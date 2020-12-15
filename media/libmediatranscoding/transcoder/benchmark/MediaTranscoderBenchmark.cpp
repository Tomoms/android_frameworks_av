/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * Native media transcoder library benchmark tests.
 *
 * How to run the benchmark:
 *
 * 1. Download the media assets from http://go/transcodingbenchmark and push the directory
 *    ("TranscodingBenchmark") to /data/local/tmp.
 *
 * 2. Compile the benchmark and sync to device:
 *      $ mm -j72 && adb sync
 *
 * 3. Run:
 *      $ adb shell /data/nativetest64/MediaTranscoderBenchmark/MediaTranscoderBenchmark
 */

#include <benchmark/benchmark.h>
#include <fcntl.h>
#include <media/MediaTranscoder.h>
#include <iostream>

using namespace android;

const std::string PARAM_VIDEO_FRAME_RATE = "VideoFrameRate";

class TranscoderCallbacks : public MediaTranscoder::CallbackInterface {
public:
    virtual void onFinished(const MediaTranscoder* transcoder __unused) override {
        std::unique_lock<std::mutex> lock(mMutex);
        mFinished = true;
        mCondition.notify_all();
    }

    virtual void onError(const MediaTranscoder* transcoder __unused,
                         media_status_t error) override {
        std::unique_lock<std::mutex> lock(mMutex);
        mFinished = true;
        mStatus = error;
        mCondition.notify_all();
    }

    virtual void onProgressUpdate(const MediaTranscoder* transcoder __unused,
                                  int32_t progress __unused) override {}

    virtual void onCodecResourceLost(const MediaTranscoder* transcoder __unused,
                                     const std::shared_ptr<ndk::ScopedAParcel>& pausedState
                                             __unused) override {}

    bool waitForTranscodingFinished() {
        std::unique_lock<std::mutex> lock(mMutex);
        while (!mFinished) {
            if (mCondition.wait_for(lock, std::chrono::minutes(5)) == std::cv_status::timeout) {
                return false;
            }
        }
        return true;
    }

    media_status_t mStatus = AMEDIA_OK;

private:
    std::mutex mMutex;
    std::condition_variable mCondition;
    bool mFinished = false;
};

static AMediaFormat* CreateDefaultVideoFormat() {
    // Default bitrate
    static constexpr int32_t kVideoBitRate = 20 * 1000 * 1000;  // 20Mbs

    AMediaFormat* videoFormat = AMediaFormat_new();
    AMediaFormat_setInt32(videoFormat, AMEDIAFORMAT_KEY_BIT_RATE, kVideoBitRate);
    return videoFormat;
}

/**
 * Callback to configure tracks for transcoding.
 * @param mime The source track mime type.
 * @param dstFormat The destination format if the track should be transcoded or nullptr if the track
 * should be passed through.
 * @return True if the track should be included in the output file.
 */
using TrackSelectionCallback = std::function<bool(const char* mime, AMediaFormat** dstFormat)>;

static void TranscodeMediaFile(benchmark::State& state, const std::string& srcFileName,
                               const std::string& dstFileName,
                               TrackSelectionCallback trackSelectionCallback) {
    // Write-only, create file if non-existent.
    static constexpr int kDstOpenFlags = O_WRONLY | O_CREAT;
    // User R+W permission.
    static constexpr int kDstFileMode = S_IRUSR | S_IWUSR;
    // Asset directory
    static const std::string kAssetDirectory = "/data/local/tmp/TranscodingBenchmark/";

    int srcFd = 0;
    int dstFd = 0;

    std::string srcPath = kAssetDirectory + srcFileName;
    std::string dstPath = kAssetDirectory + dstFileName;

    auto callbacks = std::make_shared<TranscoderCallbacks>();
    media_status_t status = AMEDIA_OK;

    if ((srcFd = open(srcPath.c_str(), O_RDONLY)) < 0) {
        state.SkipWithError("Unable to open source file");
        goto exit;
    }
    if ((dstFd = open(dstPath.c_str(), kDstOpenFlags, kDstFileMode)) < 0) {
        state.SkipWithError("Unable to open destination file");
        goto exit;
    }

    for (auto _ : state) {
        auto transcoder = MediaTranscoder::create(callbacks);

        status = transcoder->configureSource(srcFd);
        if (status != AMEDIA_OK) {
            state.SkipWithError("Unable to configure transcoder source");
            goto exit;
        }

        status = transcoder->configureDestination(dstFd);
        if (status != AMEDIA_OK) {
            state.SkipWithError("Unable to configure transcoder destination");
            goto exit;
        }

        std::vector<std::shared_ptr<AMediaFormat>> trackFormats = transcoder->getTrackFormats();
        for (int i = 0; i < trackFormats.size(); ++i) {
            AMediaFormat* srcFormat = trackFormats[i].get();
            AMediaFormat* dstFormat = nullptr;

            const char* mime = nullptr;
            if (!AMediaFormat_getString(srcFormat, AMEDIAFORMAT_KEY_MIME, &mime)) {
                state.SkipWithError("Source track format does not have MIME type");
                goto exit;
            }

            if (strncmp(mime, "video/", 6) == 0) {
                int32_t frameCount;
                if (AMediaFormat_getInt32(srcFormat, AMEDIAFORMAT_KEY_FRAME_COUNT, &frameCount)) {
                    state.counters[PARAM_VIDEO_FRAME_RATE] =
                            benchmark::Counter(frameCount, benchmark::Counter::kIsRate);
                }
            }

            if (trackSelectionCallback(mime, &dstFormat)) {
                status = transcoder->configureTrackFormat(i, dstFormat);
            }

            if (dstFormat != nullptr) {
                AMediaFormat_delete(dstFormat);
            }
            if (status != AMEDIA_OK) {
                state.SkipWithError("Unable to configure track");
                goto exit;
            }
        }

        status = transcoder->start();
        if (status != AMEDIA_OK) {
            state.SkipWithError("Unable to start transcoder");
            goto exit;
        }

        if (!callbacks->waitForTranscodingFinished()) {
            transcoder->cancel();
            state.SkipWithError("Transcoder timed out");
            goto exit;
        }
        if (callbacks->mStatus != AMEDIA_OK) {
            state.SkipWithError("Transcoder error when running");
            goto exit;
        }
    }

exit:
    if (srcFd > 0) close(srcFd);
    if (dstFd > 0) close(dstFd);
}

/**
 * Callback to edit track format for transcoding.
 * @param dstFormat The default track format for the track type.
 */
using TrackFormatEditCallback = std::function<void(AMediaFormat* dstFormat)>;

static void TranscodeMediaFile(benchmark::State& state, const std::string& srcFileName,
                               const std::string& dstFileName, bool includeAudio,
                               bool transcodeVideo,
                               const TrackFormatEditCallback& videoFormatEditor = nullptr) {
    TranscodeMediaFile(state, srcFileName, dstFileName,
                       [=](const char* mime, AMediaFormat** dstFormatOut) -> bool {
                           *dstFormatOut = nullptr;
                           if (strncmp(mime, "video/", 6) == 0 && transcodeVideo) {
                               *dstFormatOut = CreateDefaultVideoFormat();
                               if (videoFormatEditor != nullptr) {
                                   videoFormatEditor(*dstFormatOut);
                               }
                           } else if (strncmp(mime, "audio/", 6) == 0 && !includeAudio) {
                               return false;
                           }
                           return true;
                       });
}

static void SetMaxOperatingRate(AMediaFormat* format) {
    AMediaFormat_setFloat(format, AMEDIAFORMAT_KEY_OPERATING_RATE, INT32_MAX);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_PRIORITY, 1);
}

//-------------------------------- AVC to AVC Benchmarks -------------------------------------------

static void BM_TranscodeAvc2AvcAudioVideo2AudioVideo(benchmark::State& state) {
    TranscodeMediaFile(state, "video_1920x1080_3648frame_h264_22Mbps_30fps_aac.mp4",
                       "video_1920x1080_3648frame_h264_22Mbps_30fps_aac_transcoded_AV.mp4",
                       true /* includeAudio */, true /* transcodeVideo */);
}

static void BM_TranscodeAvc2AvcVideo2Video(benchmark::State& state) {
    TranscodeMediaFile(state, "video_1920x1080_3648frame_h264_22Mbps_30fps.mp4",
                       "video_1920x1080_3648frame_h264_22Mbps_30fps_transcoded_V.mp4",
                       false /* includeAudio */, true /* transcodeVideo */);
}

static void BM_TranscodeAvc2AvcAV2AVMaxOperatingRate(benchmark::State& state) {
    TranscodeMediaFile(state, "video_1920x1080_3648frame_h264_22Mbps_30fps_aac.mp4",
                       "video_1920x1080_3648frame_h264_22Mbps_30fps_aac_transcoded_AV.mp4",
                       true /* includeAudio */, true /* transcodeVideo */, SetMaxOperatingRate);
}

static void BM_TranscodeAvc2AvcV2VMaxOperatingRate(benchmark::State& state) {
    TranscodeMediaFile(state, "video_1920x1080_3648frame_h264_22Mbps_30fps.mp4",
                       "video_1920x1080_3648frame_h264_22Mbps_30fps_transcoded_V.mp4",
                       false /* includeAudio */, true /* transcodeVideo */, SetMaxOperatingRate);
}

static void BM_TranscodeAvc2AvcAV2AV720P(benchmark::State& state) {
    TranscodeMediaFile(state, "video_1280x720_3648frame_h264_16Mbps_30fps_aac.mp4",
                       "video_1280x720_3648frame_h264_16Mbps_30fps_aac_transcoded_AV.mp4",
                       true /* includeAudio */, true /* transcodeVideo */);
}

static void BM_TranscodeAvc2AvcAV2AV720PMaxOperatingRate(benchmark::State& state) {
    TranscodeMediaFile(state, "video_1280x720_3648frame_h264_16Mbps_30fps_aac.mp4",
                       "video_1280x720_3648frame_h264_16Mbps_30fps_aac_transcoded_AV.mp4",
                       true /* includeAudio */, true /* transcodeVideo */, SetMaxOperatingRate);
}
//-------------------------------- HEVC to AVC Benchmarks ------------------------------------------

static void BM_TranscodeHevc2AvcAudioVideo2AudioVideo(benchmark::State& state) {
    TranscodeMediaFile(state, "video_1920x1080_3863frame_hevc_4Mbps_30fps_aac.mp4",
                       "video_1920x1080_3863frame_hevc_4Mbps_30fps_aac_transcoded_AV.mp4",
                       true /* includeAudio */, true /* transcodeVideo */);
}

static void BM_TranscodeHevc2AvcVideo2Video(benchmark::State& state) {
    TranscodeMediaFile(state, "video_1920x1080_3863frame_hevc_4Mbps_30fps.mp4",
                       "video_1920x1080_3863frame_hevc_4Mbps_30fps_transcoded_V.mp4",
                       false /* includeAudio */, true /* transcodeVideo */);
}

static void BM_TranscodeHevc2AvcAV2AVMaxOperatingRate(benchmark::State& state) {
    TranscodeMediaFile(state, "video_1920x1080_3863frame_hevc_4Mbps_30fps_aac.mp4",
                       "video_1920x1080_3863frame_hevc_4Mbps_30fps_aac_transcoded_AV.mp4",
                       true /* includeAudio */, true /* transcodeVideo */, SetMaxOperatingRate);
}

static void BM_TranscodeHevc2AvcV2VMaxOperatingRate(benchmark::State& state) {
    TranscodeMediaFile(state, "video_1920x1080_3863frame_hevc_4Mbps_30fps.mp4",
                       "video_1920x1080_3863frame_hevc_4Mbps_30fps_transcoded_V.mp4",
                       false /* includeAudio */, true /* transcodeVideo */, SetMaxOperatingRate);
}

static void BM_TranscodeHevc2AvcAV2AV720P(benchmark::State& state) {
    TranscodeMediaFile(state, "video_1280x720_3863frame_hevc_16Mbps_30fps_aac.mp4",
                       "video_1280x720_3863frame_hevc_16Mbps_30fps_aac_transcoded_AV.mp4",
                       true /* includeAudio */, true /* transcodeVideo */);
}

static void BM_TranscodeHevc2AvcAV2AV720PMaxOperatingRate(benchmark::State& state) {
    TranscodeMediaFile(state, "video_1280x720_3863frame_hevc_16Mbps_30fps_aac.mp4",
                       "video_1280x720_3863frame_hevc_16Mbps_30fps_aac_transcoded_AV.mp4",
                       true /* includeAudio */, true /* transcodeVideo */, SetMaxOperatingRate);
}

//-------------------------------- Passthrough Benchmarks ------------------------------------------

static void BM_TranscodeAudioVideoPassthrough(benchmark::State& state) {
    TranscodeMediaFile(state, "video_1920x1080_3648frame_h264_22Mbps_30fps_aac.mp4",
                       "video_1920x1080_3648frame_h264_22Mbps_30fps_aac_passthrough_AV.mp4",
                       true /* includeAudio */, false /* transcodeVideo */);
}
static void BM_TranscodeVideoPassthrough(benchmark::State& state) {
    TranscodeMediaFile(state, "video_1920x1080_3648frame_h264_22Mbps_30fps.mp4",
                       "video_1920x1080_3648frame_h264_22Mbps_30fps_passthrough_AV.mp4",
                       false /* includeAudio */, false /* transcodeVideo */);
}

//-------------------------------- Benchmark Registration ------------------------------------------

// Benchmark registration wrapper for transcoding.
#define TRANSCODER_BENCHMARK(func) \
    BENCHMARK(func)->UseRealTime()->MeasureProcessCPUTime()->Unit(benchmark::kMillisecond)

TRANSCODER_BENCHMARK(BM_TranscodeAvc2AvcAudioVideo2AudioVideo);
TRANSCODER_BENCHMARK(BM_TranscodeAvc2AvcVideo2Video);
TRANSCODER_BENCHMARK(BM_TranscodeAvc2AvcAV2AVMaxOperatingRate);
TRANSCODER_BENCHMARK(BM_TranscodeAvc2AvcV2VMaxOperatingRate);
TRANSCODER_BENCHMARK(BM_TranscodeAvc2AvcAV2AV720P);
TRANSCODER_BENCHMARK(BM_TranscodeAvc2AvcAV2AV720PMaxOperatingRate);

TRANSCODER_BENCHMARK(BM_TranscodeHevc2AvcAudioVideo2AudioVideo);
TRANSCODER_BENCHMARK(BM_TranscodeHevc2AvcVideo2Video);
TRANSCODER_BENCHMARK(BM_TranscodeHevc2AvcAV2AVMaxOperatingRate);
TRANSCODER_BENCHMARK(BM_TranscodeHevc2AvcV2VMaxOperatingRate);
TRANSCODER_BENCHMARK(BM_TranscodeHevc2AvcAV2AV720P);
TRANSCODER_BENCHMARK(BM_TranscodeHevc2AvcAV2AV720PMaxOperatingRate);

TRANSCODER_BENCHMARK(BM_TranscodeAudioVideoPassthrough);
TRANSCODER_BENCHMARK(BM_TranscodeVideoPassthrough);

class CustomCsvReporter : public benchmark::BenchmarkReporter {
public:
    CustomCsvReporter() : mPrintedHeader(false) {}
    virtual bool ReportContext(const Context& context);
    virtual void ReportRuns(const std::vector<Run>& reports);

private:
    void PrintRunData(const Run& report);

    bool mPrintedHeader;
    std::vector<std::string> mHeaders = {"name", "real_time", "cpu_time", PARAM_VIDEO_FRAME_RATE};
};

bool CustomCsvReporter::ReportContext(const Context& context __unused) {
    return true;
}

void CustomCsvReporter::ReportRuns(const std::vector<Run>& reports) {
    std::ostream& Out = GetOutputStream();

    if (!mPrintedHeader) {
        // print the header
        for (auto header = mHeaders.begin(); header != mHeaders.end();) {
            Out << *header++;
            if (header != mHeaders.end()) Out << ",";
        }
        Out << "\n";
        mPrintedHeader = true;
    }

    // print results for each run
    for (const auto& run : reports) {
        PrintRunData(run);
    }
}

void CustomCsvReporter::PrintRunData(const Run& run) {
    if (run.error_occurred) {
        return;
    }
    std::ostream& Out = GetOutputStream();
    Out << run.benchmark_name() << ",";
    Out << run.GetAdjustedRealTime() << ",";
    Out << run.GetAdjustedCPUTime() << ",";
    auto frameRate = run.counters.find(PARAM_VIDEO_FRAME_RATE);
    if (frameRate == run.counters.end()) {
        Out << "NA"
            << ",";
    } else {
        Out << frameRate->second << ",";
    }
    Out << '\n';
}

int main(int argc, char** argv) {
    std::unique_ptr<benchmark::BenchmarkReporter> fileReporter;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]).find("--benchmark_out") != std::string::npos) {
            fileReporter.reset(new CustomCsvReporter);
            break;
        }
    }
    ::benchmark::Initialize(&argc, argv);
    if (::benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    ::benchmark::RunSpecifiedBenchmarks(nullptr, fileReporter.get());
}