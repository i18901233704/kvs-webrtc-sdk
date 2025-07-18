#include "Samples.h"
#include "CustomWsSignal.h"

/* 前向声明，供 onConnectionStateChange 调用 */
PVOID mediaSenderRoutine(PVOID);

extern PSampleConfiguration gSampleConfiguration;

INT32 main(INT32 argc, CHAR* argv[])
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT32 frameSize;
    PSampleConfiguration pSampleConfiguration = NULL;
    PCHAR pChannelName;
    SignalingClientMetrics signalingClientMetrics;
    signalingClientMetrics.version = SIGNALING_CLIENT_METRICS_CURRENT_VERSION;
    RTC_CODEC audioCodec = RTC_CODEC_OPUS;
    RTC_CODEC videoCodec = RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE;

    SET_INSTRUMENTED_ALLOCATORS();
    UINT32 logLevel = setLogLevel();
    printf("logLevel=%d\n", logLevel);

#ifndef _WIN32
    signal(SIGINT, sigintHandler);
#endif

#ifdef IOT_CORE_ENABLE_CREDENTIALS
    CHK_ERR((pChannelName = argc > 1 ? argv[1] : GETENV(IOT_CORE_THING_NAME)) != NULL, STATUS_INVALID_OPERATION,
            "AWS_IOT_CORE_THING_NAME must be set");
#else
    pChannelName = argc > 1 ? argv[1] : SAMPLE_CHANNEL_NAME;
#endif

    /* 改为不使用 AWS TURN 配置，避免 initializePeerConnection 调用 signalingClientGetIceConfigInfoCount 失败 */
    CHK_STATUS(createSampleConfiguration(pChannelName, SIGNALING_CHANNEL_ROLE_TYPE_MASTER, TRUE, FALSE, logLevel, &pSampleConfiguration));

    if (argc > 3) {
        if (!STRCMP(argv[3], AUDIO_CODEC_NAME_OPUS)) {
            audioCodec = RTC_CODEC_OPUS;
        }
    }

    if (argc > 4) {
        if (!STRCMP(argv[4], VIDEO_CODEC_NAME_H265)) {
            videoCodec = RTC_CODEC_H265;
        } else {
            DLOGI("[KVS Master] Defaulting to H264 as the specified codec's sample frames may not be available");
        }
    }

    // Set the audio and video handlers
    pSampleConfiguration->audioSource = sendAudioPackets;
    pSampleConfiguration->videoSource = sendVideoPackets;
    pSampleConfiguration->receiveAudioVideoSource = sampleReceiveAudioVideoFrame;
    pSampleConfiguration->audioCodec = audioCodec;
    pSampleConfiguration->videoCodec = videoCodec;

    // Configure the RTP rolling buffer sizes for the set of sample frames
    // to be smaller than the default settings (plus some extra for padding).
    if (pSampleConfiguration->videoCodec == RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE) {
        pSampleConfiguration->videoRollingBufferDurationSec = 3;
        pSampleConfiguration->videoRollingBufferBitratebps = 1.4 * 1024 * 1024;
    } else if (pSampleConfiguration->videoCodec == RTC_CODEC_H265) {
        pSampleConfiguration->videoRollingBufferDurationSec = 3;
        pSampleConfiguration->videoRollingBufferBitratebps = 462 * 1024;
    }

    if (pSampleConfiguration->audioCodec == RTC_CODEC_OPUS) {
        pSampleConfiguration->audioRollingBufferDurationSec = 3;
        pSampleConfiguration->audioRollingBufferBitratebps = 512 * 1024;
    }

    if (argc > 2 && STRNCMP(argv[2], "1", 2) == 0) {
        pSampleConfiguration->channelInfo.useMediaStorage = TRUE;
    }

#ifdef ENABLE_DATA_CHANNEL
    pSampleConfiguration->onDataChannel = onDataChannel;
#endif
    pSampleConfiguration->mediaType = SAMPLE_STREAMING_AUDIO_VIDEO;
    DLOGI("[KVS Master] Finished setting handlers");

    // Check if the samples are present

    if (videoCodec == RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE) {
        CHK_STATUS(readFrameFromDisk(NULL, &frameSize, "./h264SampleFrames/frame-0001.h264"));
        DLOGI("[KVS Master] Checked H264 sample video frame availability....available");
    } else if (videoCodec == RTC_CODEC_H265) {
        CHK_STATUS(readFrameFromDisk(NULL, &frameSize, "./h265SampleFrames/frame-0001.h265"));
        DLOGI("[KVS Master] Checked H265 sample video frame availability....available");
    }

    if (audioCodec == RTC_CODEC_OPUS) {
        CHK_STATUS(readFrameFromDisk(NULL, &frameSize, "./opusSampleFrames/sample-001.opus"));
        DLOGI("[KVS Master] Checked Opus sample audio frame availability....available");
    }

    // Initialize KVS WebRTC. This must be done before anything else, and must only be done once.
    CHK_STATUS(initKvsWebRtc());
    DLOGI("[KVS Master] KVS WebRTC initialization completed successfully");

    /* --- 使用 SampleStreamingSession to include transceivers --- */
    PSampleStreamingSession pSession = NULL;
    CHK_STATUS(createSampleStreamingSession(pSampleConfiguration, "viewer", TRUE, &pSession));
    pSampleConfiguration->sampleStreamingSessionList[pSampleConfiguration->streamingSessionCount++] = pSession;
    PRtcPeerConnection pc = pSession->pPeerConnection;

    /* 先启动媒体发送线程（包含音频和视频） */
#if 0
CHK_STATUS(THREAD_CREATE(&pSampleConfiguration->mediaSenderTid,
                         mediaSenderRoutine,
                         (PVOID) pSampleConfiguration));
#endif

    /* 再启动自定义 WebSocket 信令（阻塞直到 appTerminateFlag 置 TRUE） */
    CHK_STATUS(runWsSignalingMaster(pc, pSampleConfiguration,
             argc > 2 ? argv[2] : "ws://127.0.0.1:8080/ws", pChannelName, "master"));

    DLOGI("[KVS Master] WebSocket signaling finished");

    // Checking for termination
    CHK_STATUS(sessionCleanupWait(pSampleConfiguration));
    DLOGI("[KVS Master] Streaming session terminated");

CleanUp:

    if (retStatus != STATUS_SUCCESS) {
        DLOGE("[KVS Master] Terminated with status code 0x%08x", retStatus);
    }

    DLOGI("[KVS Master] Cleaning up....");
    if (pSampleConfiguration != NULL) {
        // Kick of the termination sequence
        ATOMIC_STORE_BOOL(&pSampleConfiguration->appTerminateFlag, TRUE);

        if (pSampleConfiguration->mediaSenderTid != INVALID_TID_VALUE) {
            THREAD_JOIN(pSampleConfiguration->mediaSenderTid, NULL);
        }

        /* metrics / freeSignalingClient 已移除 */

        retStatus = freeSampleConfiguration(&pSampleConfiguration);
        if (retStatus != STATUS_SUCCESS) {
            DLOGE("[KVS Master] freeSampleConfiguration(): operation returned status code: 0x%08x", retStatus);
        }
    }
    DLOGI("[KVS Master] Cleanup done");
    CHK_LOG_ERR(retStatus);

    RESET_INSTRUMENTED_ALLOCATORS();

    // https://www.gnu.org/software/libc/manual/html_node/Exit-Status.html
    // We can only return with 0 - 127. Some platforms treat exit code >= 128
    // to be a success code, which might give an unintended behaviour.
    // Some platforms also treat 1 or 0 differently, so it's better to use
    // EXIT_FAILURE and EXIT_SUCCESS macros for portability.
    return STATUS_FAILED(retStatus) ? EXIT_FAILURE : EXIT_SUCCESS;
}

STATUS readFrameFromDisk(PBYTE pFrame, PUINT32 pSize, PCHAR frameFilePath)
{
    STATUS retStatus = STATUS_SUCCESS;
    UINT64 size = 0;
    CHK_ERR(pSize != NULL, STATUS_NULL_ARG, "[KVS Master] Invalid file size");
    size = *pSize;
    // Get the size and read into frame
    CHK_STATUS(readFile(frameFilePath, TRUE, pFrame, &size));
CleanUp:

    if (pSize != NULL) {
        *pSize = (UINT32) size;
    }

    return retStatus;
}

PVOID sendVideoPackets(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) args;
    RtcEncoderStats encoderStats;
    Frame frame;
    UINT32 fileIndex = 0, frameSize;
    CHAR filePath[MAX_PATH_LEN + 1];
    STATUS status;
    UINT32 i;
    UINT64 startTime, lastFrameTime, elapsed;
    MEMSET(&encoderStats, 0x00, SIZEOF(RtcEncoderStats));
    CHK_ERR(pSampleConfiguration != NULL, STATUS_NULL_ARG, "[KVS Master] Streaming session is NULL");

    frame.presentationTs = 0;
    startTime = GETTIME();
    lastFrameTime = startTime;

    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        fileIndex = fileIndex % NUMBER_OF_H264_FRAME_FILES + 1;
        if (pSampleConfiguration->videoCodec == RTC_CODEC_H264_PROFILE_42E01F_LEVEL_ASYMMETRY_ALLOWED_PACKETIZATION_MODE) {
            SNPRINTF(filePath, MAX_PATH_LEN, "./h264SampleFrames/frame-%04d.h264", fileIndex);
        } else if (pSampleConfiguration->videoCodec == RTC_CODEC_H265) {
            SNPRINTF(filePath, MAX_PATH_LEN, "./h265SampleFrames/frame-%04d.h265", fileIndex);
        }

        CHK_STATUS(readFrameFromDisk(NULL, &frameSize, filePath));

        // Re-alloc if needed
        if (frameSize > pSampleConfiguration->videoBufferSize) {
            pSampleConfiguration->pVideoFrameBuffer = (PBYTE) MEMREALLOC(pSampleConfiguration->pVideoFrameBuffer, frameSize);
            CHK_ERR(pSampleConfiguration->pVideoFrameBuffer != NULL, STATUS_NOT_ENOUGH_MEMORY, "[KVS Master] Failed to allocate video frame buffer");
            pSampleConfiguration->videoBufferSize = frameSize;
        }

        frame.frameData = pSampleConfiguration->pVideoFrameBuffer;
        frame.size = frameSize;

        CHK_STATUS(readFrameFromDisk(frame.frameData, &frameSize, filePath));

        // based on bitrate of samples/h264SampleFrames/frame-*
        encoderStats.width = 640;
        encoderStats.height = 480;
        encoderStats.targetBitrate = 262000;
        frame.presentationTs += SAMPLE_VIDEO_FRAME_DURATION;
        printf("[Video] idx=%u size=%u ts=%" PRIu64 "\n", fileIndex, frameSize, frame.presentationTs);
        MUTEX_LOCK(pSampleConfiguration->streamingSessionListReadLock);
        for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
            status = writeFrame(pSampleConfiguration->sampleStreamingSessionList[i]->pVideoRtcRtpTransceiver, &frame);
            if (pSampleConfiguration->sampleStreamingSessionList[i]->firstFrame && status == STATUS_SUCCESS) {
                PROFILE_WITH_START_TIME(pSampleConfiguration->sampleStreamingSessionList[i]->offerReceiveTime, "Time to first frame");
                pSampleConfiguration->sampleStreamingSessionList[i]->firstFrame = FALSE;
            }
            encoderStats.encodeTimeMsec = 4; // update encode time to an arbitrary number to demonstrate stats update
            updateEncoderStats(pSampleConfiguration->sampleStreamingSessionList[i]->pVideoRtcRtpTransceiver, &encoderStats);
            if (status != STATUS_SRTP_NOT_READY_YET) {
                if (status != STATUS_SUCCESS) {
                    DLOGV("writeFrame() failed with 0x%08x", status);
                }
            } else {
                // Reset file index to ensure first frame sent upon SRTP ready is a key frame.
                fileIndex = 0;
            }
        }
        MUTEX_UNLOCK(pSampleConfiguration->streamingSessionListReadLock);

        // Adjust sleep in the case the sleep itself and writeFrame take longer than expected. Since sleep makes sure that the thread
        // will be paused at least until the given amount, we can assume that there's no too early frame scenario.
        // Also, it's very unlikely to have a delay greater than SAMPLE_VIDEO_FRAME_DURATION, so the logic assumes that this is always
        // true for simplicity.
        elapsed = lastFrameTime - startTime;
        THREAD_SLEEP(SAMPLE_VIDEO_FRAME_DURATION - elapsed % SAMPLE_VIDEO_FRAME_DURATION);
        lastFrameTime = GETTIME();
    }

CleanUp:
    DLOGI("[KVS Master] Closing video thread");
    CHK_LOG_ERR(retStatus);

    return (PVOID) (ULONG_PTR) retStatus;
}

PVOID sendAudioPackets(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleConfiguration pSampleConfiguration = (PSampleConfiguration) args;
    Frame frame;
    UINT32 fileIndex = 0, frameSize;
    CHAR filePath[MAX_PATH_LEN + 1];
    UINT32 i;
    STATUS status;

    CHK_ERR(pSampleConfiguration != NULL, STATUS_NULL_ARG, "[KVS Master] Streaming session is NULL");
    frame.presentationTs = 0;

    while (!ATOMIC_LOAD_BOOL(&pSampleConfiguration->appTerminateFlag)) {
        fileIndex = fileIndex % NUMBER_OF_OPUS_FRAME_FILES + 1;

        if (pSampleConfiguration->audioCodec == RTC_CODEC_OPUS) {
            SNPRINTF(filePath, MAX_PATH_LEN, "./opusSampleFrames/sample-%03d.opus", fileIndex);
        }

        CHK_STATUS(readFrameFromDisk(NULL, &frameSize, filePath));

        // Re-alloc if needed
        if (frameSize > pSampleConfiguration->audioBufferSize) {
            pSampleConfiguration->pAudioFrameBuffer = (UINT8*) MEMREALLOC(pSampleConfiguration->pAudioFrameBuffer, frameSize);
            CHK_ERR(pSampleConfiguration->pAudioFrameBuffer != NULL, STATUS_NOT_ENOUGH_MEMORY, "[KVS Master] Failed to allocate audio frame buffer");
            pSampleConfiguration->audioBufferSize = frameSize;
        }

        frame.frameData = pSampleConfiguration->pAudioFrameBuffer;
        frame.size = frameSize;

        CHK_STATUS(readFrameFromDisk(frame.frameData, &frameSize, filePath));

        frame.presentationTs += SAMPLE_AUDIO_FRAME_DURATION;
        DLOGI("[Audio] idx=%u size=%u ts=%" PRIu64, fileIndex, frameSize, frame.presentationTs);

        MUTEX_LOCK(pSampleConfiguration->streamingSessionListReadLock);
        for (i = 0; i < pSampleConfiguration->streamingSessionCount; ++i) {
            status = writeFrame(pSampleConfiguration->sampleStreamingSessionList[i]->pAudioRtcRtpTransceiver, &frame);
            if (status != STATUS_SRTP_NOT_READY_YET) {
                if (status != STATUS_SUCCESS) {
                    DLOGV("writeFrame() failed with 0x%08x", status);
                } else if (pSampleConfiguration->sampleStreamingSessionList[i]->firstFrame && status == STATUS_SUCCESS) {
                    PROFILE_WITH_START_TIME(pSampleConfiguration->sampleStreamingSessionList[i]->offerReceiveTime, "Time to first frame");
                    pSampleConfiguration->sampleStreamingSessionList[i]->firstFrame = FALSE;
                }
            } else {
                // Reset file index to stay in sync with video frames.
                fileIndex = 0;
            }
        }
        MUTEX_UNLOCK(pSampleConfiguration->streamingSessionListReadLock);
        THREAD_SLEEP(SAMPLE_AUDIO_FRAME_DURATION);
    }

CleanUp:
    DLOGI("[KVS Master] closing audio thread");
    return (PVOID) (ULONG_PTR) retStatus;
}

PVOID sampleReceiveAudioVideoFrame(PVOID args)
{
    STATUS retStatus = STATUS_SUCCESS;
    PSampleStreamingSession pSampleStreamingSession = (PSampleStreamingSession) args;
    CHK_ERR(pSampleStreamingSession != NULL, STATUS_NULL_ARG, "[KVS Master] Streaming session is NULL");
    CHK_STATUS(transceiverOnFrame(pSampleStreamingSession->pVideoRtcRtpTransceiver, (UINT64) pSampleStreamingSession, sampleVideoFrameHandler));
    CHK_STATUS(transceiverOnFrame(pSampleStreamingSession->pAudioRtcRtpTransceiver, (UINT64) pSampleStreamingSession, sampleAudioFrameHandler));

CleanUp:

    return (PVOID) (ULONG_PTR) retStatus;
}
