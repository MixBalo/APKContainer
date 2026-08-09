/*
 * opensl_bridge.mm — OpenSL ES → AVAudioEngine PCM routing (Objective-C++)
 *
 * Status: REAL (common path). AVAudioEngine + AVAudioPlayerNode + ring buffer
 *         actually produces sound for the SLBufferQueue PCM source path:
 *           opensl_bridge_engine_create   -> AVAudioEngine
 *           opensl_bridge_create_player   -> AVAudioPlayerNode + AVAudioFormat
 *                                            (signed-int16 PCM interleaved, OR
 *                                             float32 non-interleaved) +
 *                                            AVAudioConverter if the app's
 *                                            format differs from the engine's
 *                                            main-mixer output format.
 *           opensl_bridge_player_enqueue   -> AVAudioPCMBuffer (memcpy) +
 *                                            scheduleBuffer:atTime:options:
 *                                            completionHandler:; the completion
 *                                            handler re-pulls from the per-
 *                                            player pending queue and fires
 *                                            the SLES buffer-queue callback.
 *           opensl_bridge_player_play/stop -> AVAudioPlayerNode play / pause.
 *           opensl_bridge_player_destroy   -> stop + detach from engine.
 *
 *         Recorder (mic input) is PARTIAL: an AVAudioInputNode tap accumulates
 *         float32 samples into a ring buffer the app drains via
 *         opensl_bridge_recorder_read. No format conversion yet (caller gets
 *         float32 non-interleaved at the requested sample_rate/channels).
 *
 *         AAudio is unsupported: aaudio_stub_unimplemented returns
 *         AAUDIO_ERROR_UNIMPLEMENTED.
 *
 *         3D positional audio (SL3DLocation, SLBassBoost, SLEffectSend,
 *         SLEnvironmentalReverb, etc.) is NOT implemented: those IIDs are
 *         not exposed; apps requesting them get nothing (logged at WARN by
 *         the SLES shim in bionic_shim.c if reached via slCreateEngine).
 *
 * Honesty: a typical Android app that creates an AudioTrack-style buffer queue
 *         with signed-16-bit PCM and writes data will produce sound on the
 *         device. Apps that exercise the SLES C API directly (slCreateEngine +
 *         Engine.CreateAudioPlayer + BufferQueue.Enqueue) also work via the
 *         minimal SLES wrapper in bionic_shim.c which forwards to these
 *         functions.
 *
 * Build: compile as Objective-C++ (.mm) with ARC enabled, C++17 (gnu++20 in
 *        project.yml). XcodeGen picks up .mm files in Native/ automatically
 *        (project.yml sources glob: `path: Native`).
 *
 * See docs/ARCHITECTURE.md §5, docs/CAPABILITY_MATRIX.md §5.
 */

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include "opensl_bridge.h"
#include "log_file.h"

#include <vector>
#include <string>
#include <mutex>
#include <atomic>

#define TAG "opensl"

// ============================================================================
// Engine
// ============================================================================

struct sl_engine {
    AVAudioEngine *audioEngine = nil;
    // Serial queue for any engine-wide mutations (player attach/detach).
    dispatch_queue_t mutationQueue = nil;
};

// Global engine created by apkcontainer_audio_start(). The runtime_glue.c
// owns the lifetime (creates lazily on first audio_start, destroys on
// audio_stop). The AudioTrack Java stub (dex_interp.cpp) reaches this via
// opensl_bridge_get_global_engine().
static std::atomic<sl_engine_t *> g_global_engine{nullptr};

sl_engine_t *opensl_bridge_get_global_engine(void) {
    return g_global_engine.load(std::memory_order_acquire);
}

// ---- AVAudioSession helper ----
// Configures the session for ambient playback (mixes with other audio) and
// activates it. Idempotent. Failures are logged but do NOT abort — the engine
// may still produce audio if the session was already configured by Swift's
// AudioBridge.swift.
static void opensl_ensure_audio_session(void) {
    @try {
        AVAudioSession *session = [AVAudioSession sharedInstance];
        NSError *err = nil;
        // .ambient + .mixWithOthers is the least invasive category — apps
        // that need .playback can call setCategory themselves later.
        if (![session.category isEqualToString:AVAudioSessionCategoryAmbient]) {
            [session setCategory:AVAudioSessionCategoryAmbient
                      withOptions:AVAudioSessionCategoryOptionMixWithOthers
                            error:&err];
            if (err) {
                LOGW(TAG, "AVAudioSession setCategory failed: %s",
                     err.localizedDescription.UTF8String);
                err = nil;
            }
        }
        if (!session.isOtherAudioPlaying) {
            [session setActive:YES error:&err];
            if (err) {
                LOGW(TAG, "AVAudioSession setActive failed: %s",
                     err.localizedDescription.UTF8String);
            }
        }
    } @catch (NSException *ex) {
        LOGW(TAG, "AVAudioSession setup exception: %s", ex.description.UTF8String);
    }
}

int opensl_bridge_engine_create(sl_engine_t **out_engine) {
    if (!out_engine) return -1;
    opensl_ensure_audio_session();

    sl_engine_t *e = new sl_engine_t();
    e->audioEngine = [[AVAudioEngine alloc] init];
    e->mutationQueue = dispatch_queue_create("opensl.engine",
                                             DISPATCH_QUEUE_SERIAL);
    if (!e->audioEngine) {
        LOGE(TAG, "engine_create: AVAudioEngine alloc failed");
        delete e;
        return -2;
    }
    // Touch the mainMixerNode so the audio graph is initialized before any
    // player connects to it. This forces creation of the mixer + the implicit
    // mixer→output connection.
    AVAudioMixerNode *mixer = e->audioEngine.mainMixerNode;
    (void)mixer;
    LOGI(TAG, "engine_create: AVAudioEngine=%p (outputHWFormat: %s)",
         e->audioEngine,
         [e->audioEngine.mainMixerNode outputFormatForBus:0]
            .description.UTF8String ?: "?");
    *out_engine = e;
    return 0;
}

int opensl_bridge_engine_destroy(sl_engine_t *engine) {
    if (!engine) return 0;
    @try {
        if (engine->audioEngine && engine->audioEngine.running) {
            [engine->audioEngine stop];
        }
    } @catch (NSException *ex) {
        LOGW(TAG, "engine_destroy: exception while stopping: %s",
             ex.description.UTF8String);
    }
    // If this was the global engine, clear the pointer.
    sl_engine_t *expected = engine;
    g_global_engine.compare_exchange_strong(expected, nullptr);
    engine->audioEngine = nil;
    engine->mutationQueue = nil;   // ARC releases
    delete engine;
    LOGI(TAG, "engine_destroy: ok");
    return 0;
}

// ============================================================================
// Player (and Recorder, since they share the sl_player_t handle)
// ============================================================================

struct sl_player {
    sl_engine_t *engine = nil;

    // ---- Player (output) ----
    AVAudioPlayerNode *node = nil;
    AVAudioFormat      *appFormat = nil;   // PCM format the app feeds us
    AVAudioFormat      *outFormat = nil;   // mainMixer input format we connect with
    AVAudioConverter   *converter = nil;   // appFormat -> outFormat; nil if equal

    // ---- Ring buffer / scheduling ----
    // A FIFO of converted AVAudioPCMBuffer chunks ready to be scheduled. All
    // access happens on `queue` (serial). scheduleNextLocked is always called
    // on the queue.
    dispatch_queue_t queue = nil;
    NSMutableArray<AVAudioPCMBuffer *> *pending = nil;
    bool playing = false;
    bool inFlight = false;       // a buffer is currently scheduled

    // Lifetime management: the player struct is freed in
    // opensl_bridge_player_destroy, but AVAudioPlayerNode.scheduleBuffer's
    // completionHandler runs asynchronously on the audio render thread and
    // captures the player pointer. We use a dispatch_group to keep the
    // player alive until every in-flight completionHandler has finished its
    // queue work (so destroy() can dispatch_group_wait before deleting).
    dispatch_group_t completionGroup = nil;

    // SLES buffer-queue callback (RegisterCallback). Invoked from `queue` when
    // a buffer is consumed.
    sl_buffer_callback_t callback = nullptr;
    void *callbackContext = nullptr;

    // ---- Recorder (mic) ----
    bool isRecorder = false;
    int  recChannels = 0;
    int  recSampleRate = 0;
    // The tap appends float32 non-interleaved samples here; recorder_read
    // drains them. Guarded by `queue`.
    std::vector<float> recRing;
    size_t recReadPos = 0;

    // ---- Format bookkeeping (for buildBufferFromPCM) ----
    int channels = 0;
    int sampleRate = 0;
    int bitsPerSample = 0;
};

// ---- AVAudioFormat helpers ----

static AVAudioCommonFormat commonFormatForBps(int bps) {
    // Android SLDataFormat_PCM typical: 16-bit signed int. We also accept
    // 32-bit float (SL_PCM_FORMAT_FLOAT32, Android 5.0+). 8-bit and 24-bit
    // PCM are NOT natively supported by AVAudioFormat; we coerce to int16
    // and warn.
    if (bps == 16) return AVAudioPCMFormatInt16;
    if (bps == 32) return AVAudioPCMFormatFloat32;
    return AVAudioPCMFormatInt16;
}

static BOOL interleavedForBps(int bps) {
    // int16 PCM from Android is interleaved. float32 — accept interleaved too,
    // AVAudioConverter handles either; we use interleaved to match the byte
    // layout the app writes.
    return YES;
}

// Build an AVAudioPCMBuffer in `appFormat` from a raw PCM byte blob.
// Our appFormat is always interleaved (see interleavedForBps), so the bytes
// are a single contiguous blob we can memcpy into channel 0.
static AVAudioPCMBuffer *buildBufferFromAppPCM(sl_player_t *p,
                                               const void *data, size_t bytes) {
    if (!p->appFormat) return nil;
    const AudioStreamBasicDescription *asbd =
        [p->appFormat streamDescription];
    size_t bytesPerFrame = asbd->mBytesPerFrame;
    if (bytesPerFrame == 0) return nil;
    AVAudioFrameCount frameCount = (AVAudioFrameCount)(bytes / bytesPerFrame);
    if (frameCount == 0) return nil;

    AVAudioPCMBuffer *buf = [[AVAudioPCMBuffer alloc]
        initWithPCMFormat:p->appFormat frameCapacity:frameCount];
    if (!buf) return nil;
    buf.frameLength = frameCount;

    // Pick the right channel-data pointer for the common format.
    void *dst = NULL;
    switch (p->appFormat.commonFormat) {
        case AVAudioPCMFormatInt16:   dst = buf.int16ChannelData[0]; break;
        case AVAudioPCMFormatFloat32: dst = buf.floatChannelData[0]; break;
        case AVAudioPCMFormatInt32:   dst = buf.int32ChannelData[0]; break;
        default:
            LOGW(TAG, "buildBufferFromAppPCM: unsupported commonFormat %lu",
                 (unsigned long)p->appFormat.commonFormat);
            return nil;
    }
    if (!dst) return nil;
    memcpy(dst, data, bytes);
    return buf;
}

// Convert an app-format buffer to the engine's output format via
// AVAudioConverter. Returns a retained buffer (may be the same as input if
// no conversion is needed or conversion fails — caller always releases via
// autorelease).
static AVAudioPCMBuffer *convertToOutputFormat(sl_player_t *p,
                                               AVAudioPCMBuffer *inBuf) {
    if (!p->converter) return inBuf;

    double ratio = p->outFormat.sampleRate / p->appFormat.sampleRate;
    if (ratio < 1.0) ratio = 1.0;
    AVAudioFrameCount outCap =
        (AVAudioFrameCount)((double)inBuf.frameLength * ratio + 64.0);

    AVAudioPCMBuffer *outBuf = [[AVAudioPCMBuffer alloc]
        initWithPCMFormat:p->outFormat frameCapacity:outCap];
    if (!outBuf) return inBuf;

    NSError *err = nil;
    __block BOOL inputDone = NO;
    BOOL ok = [p->converter convertToBuffer:outBuf
                                      error:&err
                          withInputFromBlock:^AVAudioBuffer *(
                              AVAudioPacketCount inNPackets,
                              AVAudioConverterInputStatus *outStatus) {
        if (inputDone || inNPackets == 0) {
            *outStatus = AVAudioConverterInputStatus_EndOfStream;
            return nil;
        }
        inputDone = YES;
        *outStatus = AVAudioConverterInputStatus_HaveData;
        return inBuf;
    }];
    if (!ok || err) {
        LOGW(TAG, "convertToOutputFormat: failed (%s) — falling back to appFormat",
             err.localizedDescription.UTF8String);
        return inBuf;
    }
    return outBuf;
}

// Schedule the next pending buffer. MUST be called on p->queue.
static void scheduleNextLocked(sl_player_t *p) {
    if (!p || !p->node) return;
    if (p->inFlight) return;            // already one in flight
    if (!p->playing) return;            // not playing
    if (p->pending.count == 0) return;  // nothing queued

    AVAudioPCMBuffer *buf = p->pending[0];
    [p->pending removeObjectAtIndex:0];
    p->inFlight = true;
    if (p->completionGroup) dispatch_group_enter(p->completionGroup);

    // Raw pointer capture — the player struct is kept alive by
    // completionGroup (destroy() waits for all in-flight completions before
    // deleting). __weak doesn't apply to C++ structs.
    sl_player_t *raw = p;
    [p->node scheduleBuffer:buf
                     atTime:nil
                     options:0
            completionHandler:^{
        // The completion fires on the audio render thread. Hop to our
        // serial queue to mutate state.
        dispatch_async(raw->queue, ^{
            raw->inFlight = false;
            // Fire the SLES buffer-queue callback so the app can Enqueue more.
            if (raw->callback) {
                @try {
                    raw->callback(raw->callbackContext);
                } @catch (NSException *ex) {
                    LOGW(TAG, "buffer callback threw: %s",
                         ex.description.UTF8String);
                }
            }
            // Schedule the next chunk if any.
            scheduleNextLocked(raw);
            if (raw->completionGroup) dispatch_group_leave(raw->completionGroup);
        });
    }];
}

// ---- Player API ----

int opensl_bridge_create_player(sl_engine_t *engine, int channels,
                                int sample_rate, int bits_per_sample,
                                sl_player_t **out_player) {
    if (!out_player) return -1;
    if (!engine || !engine->audioEngine) return -2;

    // Validate / clamp format parameters.
    if (channels < 1 || channels > 8) {
        LOGW(TAG, "create_player: bad channels=%d (clamped to 2)", channels);
        channels = 2;
    }
    if (sample_rate < 8000 || sample_rate > 192000) {
        LOGW(TAG, "create_player: bad sample_rate=%d (clamped to 44100)",
             sample_rate);
        sample_rate = 44100;
    }
    if (bits_per_sample != 16 && bits_per_sample != 32) {
        LOGW(TAG, "create_player: unsupported bits_per_sample=%d (forcing 16)",
             bits_per_sample);
        bits_per_sample = 16;
    }

    sl_player_t *p = new sl_player_t();
    p->engine = engine;
    p->channels = channels;
    p->sampleRate = sample_rate;
    p->bitsPerSample = bits_per_sample;
    p->queue = dispatch_queue_create("opensl.player", DISPATCH_QUEUE_SERIAL);
    p->pending = [NSMutableArray new];
    p->completionGroup = dispatch_group_create();

    // App PCM format (interleaved int16 by default; float32 if requested).
    BOOL interleaved = interleavedForBps(bits_per_sample);
    AVAudioCommonFormat cfmt = commonFormatForBps(bits_per_sample);
    p->appFormat = [[AVAudioFormat alloc]
        initWithCommonFormat:cfmt
                  sampleRate:(double)sample_rate
                    channels:(AVAudioChannelCount)channels
                 interleaved:interleaved];
    if (!p->appFormat) {
        LOGE(TAG, "create_player: AVAudioFormat init failed (ch=%d sr=%d bps=%d)",
             channels, sample_rate, bits_per_sample);
        delete p;
        return -3;
    }

    // Output format = mainMixer's input bus format (the engine's native
    // rendering format, typically non-interleaved float32 at the hardware
    // sample rate, 1 or 2 channels).
    p->outFormat = [engine->audioEngine.mainMixerNode
        outputFormatForBus:0];
    if (!p->outFormat || p->outFormat.sampleRate == 0) {
        // Fallback: synthesize a sane default (44.1k stereo non-interleaved float).
        p->outFormat = [[AVAudioFormat alloc]
            initWithCommonFormat:AVAudioPCMFormatFloat32
                      sampleRate:44100.0
                        channels:2
                     interleaved:NO];
    }

    // If app format != output format, create a converter. We schedule
    // converted buffers; the player node's output connection is the engine's
    // output format, so the mixer doesn't need to do extra conversion.
    if (![p->appFormat isEqual:p->outFormat]) {
        p->converter = [[AVAudioConverter alloc]
            initFromFormat:p->appFormat toFormat:p->outFormat];
        if (!p->converter) {
            LOGW(TAG, "create_player: AVAudioConverter init failed; "
                      "will connect with appFormat and let mixer convert");
        }
    }

    // Create + attach the player node.
    p->node = [[AVAudioPlayerNode alloc] init];
    [engine->audioEngine attachNode:p->node];

    // Connect player -> mainMixer. Use the output format if we have a
    // converter (so scheduled buffers — which are already converted — flow
    // unchanged into the mixer). Otherwise use the appFormat and let the
    // mixer handle sample-rate / channel conversion.
    AVAudioFormat *connectFmt = p->converter ? p->outFormat : p->appFormat;
    NSError *err = nil;
    [engine->audioEngine connect:p->node
                              to:engine->audioEngine.mainMixerNode
                          format:connectFmt];

    // Start the engine if not already running.
    if (!engine->audioEngine.running) {
        err = nil;
        @try {
            [engine->audioEngine prepare];
            BOOL started = [engine->audioEngine startAndReturnError:&err];
            if (!started || err) {
                LOGE(TAG, "create_player: engine start failed: %s",
                     err.localizedDescription.UTF8String);
            } else {
                LOGI(TAG, "create_player: engine started");
            }
        } @catch (NSException *ex) {
            LOGE(TAG, "create_player: engine start exception: %s",
                 ex.description.UTF8String);
        }
    }

    LOGI(TAG, "create_player: ok ch=%d sr=%d bps=%d appFmt=%s outFmt=%s converter=%d player=%p",
         channels, sample_rate, bits_per_sample,
         p->appFormat.description.UTF8String ?: "?",
         p->outFormat.description.UTF8String ?: "?",
         p->converter ? 1 : 0,
         p);
    *out_player = p;
    return 0;
}

int opensl_bridge_player_enqueue(sl_player_t *player, const void *data,
                                 size_t bytes) {
    if (!player || !data || bytes == 0) return -1;
    if (player->isRecorder) return -1;
    if (!player->node || !player->appFormat) return -2;

    // Build the app-format buffer (memcpy).
    AVAudioPCMBuffer *appBuf = buildBufferFromAppPCM(player, data, bytes);
    if (!appBuf) {
        LOGW(TAG, "enqueue: buildBufferFromAppPCM failed (bytes=%zu)", bytes);
        return -3;
    }

    // Convert to the output format if needed.
    AVAudioPCMBuffer *schedBuf =
        player->converter ? convertToOutputFormat(player, appBuf) : appBuf;

    // Push to the pending FIFO and kick the scheduler.
    __block int rc = 0;
    dispatch_sync(player->queue, ^{
        @try {
            [player->pending addObject:schedBuf];
        } @catch (NSException *ex) {
            LOGW(TAG, "enqueue: addObject threw: %s",
                 ex.description.UTF8String);
            rc = -4;
            return;
        }
        if (player->playing) scheduleNextLocked(player);
    });
    return rc;
}

int opensl_bridge_player_set_callback(sl_player_t *player,
                                      sl_buffer_callback_t cb, void *context) {
    if (!player) return -1;
    dispatch_sync(player->queue, ^{
        player->callback = cb;
        player->callbackContext = context;
    });
    LOGD(TAG, "set_callback: %p ctx=%p", cb, context);
    return 0;
}

int opensl_bridge_player_play(sl_player_t *player) {
    if (!player) return -1;
    if (player->isRecorder) return -1;
    dispatch_sync(player->queue, ^{
        player->playing = true;
        if (player->node && !player->node.playing) {
            [player->node play];
        }
        scheduleNextLocked(player);
    });
    LOGI(TAG, "player_play: %p", player);
    return 0;
}

int opensl_bridge_player_stop(sl_player_t *player) {
    if (!player) return -1;
    if (player->isRecorder) return -1;
    dispatch_sync(player->queue, ^{
        player->playing = false;
        if (player->node) [player->node pause];
        // Note: we do NOT call reset (which would cancel scheduled buffers
        // mid-flight and drop samples the app already enqueued). Pause is
        // the SLES PAUSED state; resumed via play() continues draining the
        // pending FIFO.
    });
    LOGI(TAG, "player_stop: %p", player);
    return 0;
}

int opensl_bridge_player_destroy(sl_player_t *player) {
    if (!player) return 0;
    if (!player->isRecorder) {
        // 1. Mark as not playing + clear callbacks. Do this on the queue so
        //    scheduleNextLocked (which may already be queued) sees the
        //    updated state and doesn't schedule new buffers.
        dispatch_sync(player->queue, ^{
            player->playing = false;
            player->callback = nullptr;
            player->callbackContext = nullptr;
            [player->pending removeAllObjects];
        });
        // 2. Stop the node — synchronously cancels scheduled buffers and
        //    invokes their completionHandlers. Each does
        //    dispatch_async(queue, ...) which we'll drain next.
        AVAudioPlayerNode *node = player->node;
        if (node) {
            @try { [node stop]; }
            @catch (NSException *ex) {
                LOGW(TAG, "player_destroy: stop threw: %s",
                     ex.description.UTF8String);
            }
        }
        // 3. Wait for all queued completionHandler work to drain. After this
        //    returns, no completionHandler can touch the player.
        if (player->completionGroup) {
            dispatch_group_wait(player->completionGroup, DISPATCH_TIME_FOREVER);
        }
        // 4. Detach from engine.
        if (node && player->engine && player->engine->audioEngine) {
            @try {
                [player->engine->audioEngine detachNode:node];
            } @catch (NSException *ex) {
                LOGW(TAG, "player_destroy: detach threw: %s",
                     ex.description.UTF8String);
            }
        }
    } else {
        // Recorder cleanup: remove the tap from the input node. This
        // synchronously cancels the tap and waits for in-flight tap blocks
        // to complete. Each tap block did dispatch_async(queue, ...) — drain
        // the queue so no queued work touches the player after we delete it.
        if (player->engine && player->engine->audioEngine) {
            AVAudioInputNode *input = player->engine->audioEngine.inputNode;
            if (input) {
                @try {
                    [input removeTapOnBus:0];
                } @catch (NSException *ex) {
                    LOGW(TAG, "recorder_destroy: removeTap threw: %s",
                         ex.description.UTF8String);
                }
            }
        }
        // Drain the recorder queue.
        if (player->queue) {
            dispatch_sync(player->queue, ^{ /* drain */ });
        }
    }
    player->node = nil;
    player->appFormat = nil;
    player->outFormat = nil;
    player->converter = nil;
    player->pending = nil;
    // ARC manages dispatch_queue_t / dispatch_group_t as Obj-C objects under
    // -fobjc-arc; assigning nil releases the prior value. (The C++ destructor
    // would also release strong members, but we're explicit for clarity.)
    player->queue = nil;
    player->completionGroup = nil;
    delete player;
    LOGI(TAG, "player_destroy: ok");
    return 0;
}

// ============================================================================
// Recorder (mic input) — PARTIAL
// ============================================================================

int opensl_bridge_create_recorder(sl_engine_t *engine, int channels,
                                  int sample_rate, int bits_per_sample,
                                  sl_player_t **out_recorder) {
    if (!out_recorder) return -1;
    if (!engine || !engine->audioEngine) return -2;

    if (channels < 1 || channels > 2) {
        LOGW(TAG, "create_recorder: clamping channels %d -> 1", channels);
        channels = 1;
    }
    if (sample_rate < 8000 || sample_rate > 48000) {
        LOGW(TAG, "create_recorder: clamping sample_rate %d -> 44100",
             sample_rate);
        sample_rate = 44100;
    }
    (void)bits_per_sample;  // we always deliver float32 for now (PARTIAL).

    opensl_ensure_audio_session();

    sl_player_t *r = new sl_player_t();
    r->engine = engine;
    r->isRecorder = true;
    r->channels = channels;
    r->sampleRate = sample_rate;
    r->recChannels = channels;
    r->recSampleRate = sample_rate;
    r->queue = dispatch_queue_create("opensl.recorder", DISPATCH_QUEUE_SERIAL);

    // Install a tap on the input node. The tap delivers float32
    // non-interleaved samples at our requested sample rate. iOS will up/down
    //sample from the mic's native format.
    AVAudioInputNode *input = engine->audioEngine.inputNode;
    if (!input) {
        LOGE(TAG, "create_recorder: inputNode is nil — mic not available");
        delete r;
        return -3;
    }
    AVAudioFormat *tapFmt = [[AVAudioFormat alloc]
        initWithCommonFormat:AVAudioPCMFormatFloat32
                  sampleRate:(double)sample_rate
                    channels:(AVAudioChannelCount)channels
                 interleaved:NO];
    if (!tapFmt) {
        LOGE(TAG, "create_recorder: tap format init failed");
        delete r;
        return -4;
    }

    // Raw pointer capture — the recorder struct is kept alive until
    // removeTapOnBus synchronously cancels in-flight blocks in destroy().
    // (__weak doesn't apply to C++ structs.)
    sl_player_t *raw = r;
    @try {
        [input installTapOnBus:0
                    bufferSize:4096
                        format:tapFmt
                          block:^(AVAudioPCMBuffer *buf, AVAudioTime *when) {
            (void)when;
            // Append all channels' samples (non-interleaved -> interleave).
            AVAudioFrameCount n = buf.frameLength;
            int ch = (int)buf.format.channelCount;
            if (ch < 1) ch = raw->recChannels;
            dispatch_async(raw->queue, ^{
                for (AVAudioFrameCount i = 0; i < n; i++) {
                    for (int c = 0; c < ch; c++) {
                        if (buf.floatChannelData && buf.floatChannelData[c]) {
                            raw->recRing.push_back(buf.floatChannelData[c][i]);
                        } else {
                            raw->recRing.push_back(0.0f);
                        }
                    }
                }
                // Cap the ring at 8 MiB to avoid unbounded growth if the app
                // never reads. Drop oldest samples.
                const size_t CAP = (8u * 1024u * 1024u) / sizeof(float);
                if (raw->recRing.size() > CAP) {
                    size_t excess = raw->recRing.size() - CAP;
                    raw->recRing.erase(raw->recRing.begin(),
                                       raw->recRing.begin() + excess);
                    raw->recReadPos = (raw->recReadPos > excess)
                                       ? raw->recReadPos - excess : 0;
                    LOGW(TAG, "recorder ring overflowed %zu samples; dropped oldest",
                         excess);
                }
            });
        }];
    } @catch (NSException *ex) {
        LOGE(TAG, "create_recorder: installTap threw: %s",
             ex.description.UTF8String);
        delete r;
        return -5;
    }

    // Start the engine so the tap actually fires.
    if (!engine->audioEngine.running) {
        NSError *err = nil;
        [engine->audioEngine prepare];
        BOOL started = [engine->audioEngine startAndReturnError:&err];
        if (!started || err) {
            LOGW(TAG, "create_recorder: engine start failed: %s — tap may not fire",
                 err.localizedDescription.UTF8String);
        }
    }

    LOGI(TAG, "create_recorder: PARTIAL — mic tap installed (sr=%d ch=%d, float32)",
         sample_rate, channels);
    *out_recorder = r;
    return 0;
}

int opensl_bridge_recorder_read(sl_player_t *recorder, void *out_buf,
                                size_t bytes) {
    if (!recorder || !out_buf || !recorder->isRecorder) return -1;
    size_t floatsWanted = bytes / sizeof(float);

    __block size_t available = 0;
    __block size_t toCopy = 0;
    dispatch_sync(recorder->queue, ^{
        size_t have = recorder->recRing.size() - recorder->recReadPos;
        toCopy = (have < floatsWanted) ? have : floatsWanted;
        if (toCopy > 0) {
            memcpy(out_buf,
                   recorder->recRing.data() + recorder->recReadPos,
                   toCopy * sizeof(float));
            recorder->recReadPos += toCopy;
            // Compact if we've consumed half the buffer.
            if (recorder->recReadPos > recorder->recRing.size() / 2) {
                recorder->recRing.erase(recorder->recRing.begin(),
                                        recorder->recRing.begin() +
                                            recorder->recReadPos);
                recorder->recReadPos = 0;
            }
        }
        available = recorder->recRing.size() - recorder->recReadPos;
    });
    (void)available;
    return (int)(toCopy * sizeof(float));
}

// ============================================================================
// AAudio — unsupported
// ============================================================================

int aaudio_stub_unimplemented(void) {
    LOGW(TAG, "AAudio unsupported — returns AAUDIO_ERROR_UNIMPLEMENTED");
    return -0x7fffffff;   /* AAUDIO_ERROR_UNIMPLEMENTED */
}

// ============================================================================
// C-ABI helper used by runtime_glue.c (apkcontainer_audio_start/stop) and
// bionic_shim.c (slCreateEngine). Defined here in the .mm so it can call
// into the engine lifecycle above. Declared in opensl_bridge.h with C
// linkage (the header wraps declarations in extern "C").
// ============================================================================

void opensl_bridge_set_global_engine(sl_engine_t *engine) {
    sl_engine_t *old = g_global_engine.exchange(engine, std::memory_order_acq_rel);
    if (old) {
        // The caller is replacing an existing global engine. Destroy the old
        // one to avoid a leak (the caller has handed ownership to us).
        opensl_bridge_engine_destroy(old);
    }
}
