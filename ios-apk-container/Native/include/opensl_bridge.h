/*
 * opensl_bridge.h — OpenSL ES → AVAudioEngine
 *
 * Status: REAL (common path). The buffer-queue PCM source path
 *         (CreateAudioPlayer + Enqueue + Play) produces sound on the device
 *         via AVAudioPlayerNode → mainMixer → output. Recorder (mic) is
 *         PARTIAL — routes mic PCM into a ring buffer the app reads via a
 *         callback. AAudio is unsupported (returns AAUDIO_ERROR_UNIMPLEMENTED).
 *
 * Implementation lives in opensl_bridge.mm (Objective-C++ / ARC). This header
 * is C-only so both .c and .mm TUs can include it.
 *
 * See docs/ARCHITECTURE.md §5, docs/CAPABILITY_MATRIX.md §5.
 */
#ifndef APKCONTAINER_OPENSL_BRIDGE_H
#define APKCONTAINER_OPENSL_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handles mirror the OpenSL ES object model. */
typedef struct sl_engine  sl_engine_t;
typedef struct sl_object  sl_object_t;
typedef struct sl_player  sl_player_t;

/* ---- Engine lifecycle ----
 * Lazily creates a real AVAudioEngine. Multiple engines are allowed but the
 * common case is one global engine created by apkcontainer_audio_start(). */
int  opensl_bridge_engine_create(sl_engine_t **out_engine);
int  opensl_bridge_engine_destroy(sl_engine_t *engine);

/* Returns the global engine created by apkcontainer_audio_start(), or NULL.
 * Used by the JNI / AudioTrack Java stub to find the engine without going
 * through the SLES object model. */
sl_engine_t *opensl_bridge_get_global_engine(void);

/* Atomically swaps the global engine pointer. If `engine` is NULL, the
 * previous engine (if any) is destroyed. If `engine` is non-NULL, the
 * previous engine (if any) is also destroyed (we don't support multiple
 * global engines). The caller transfers ownership to the global slot.
 * Implemented in opensl_bridge.mm. */
void opensl_bridge_set_global_engine(sl_engine_t *engine);

/* ---- Player (CreateOutputMix + CreateAudioPlayer with bufferQueue) ----
 * PCM format: channels (1..8), sampleRate (8000..192000), bitsPerSample
 * (16 = signed-int16 PCM; 32 = float32 PCM). */
int  opensl_bridge_create_player(sl_engine_t *engine,
                                 int channels, int sample_rate, int bits_per_sample,
                                 sl_player_t **out_player);

/* Enqueue PCM bytes for playback. The data is COPIED into an AVAudioPCMBuffer
 * (the caller may free/reuse `data` on return). The SLES buffer-queue
 * callback (set via opensl_bridge_player_set_callback) is fired from the
 * audio render thread once the buffer has been consumed, so the app can
 * Enqueue more. */
int  opensl_bridge_player_enqueue(sl_player_t *player,
                                  const void *data, size_t bytes);

/* Set the buffer-queue callback (Android's SLBufferQueueItf::RegisterCallback).
 * Called when a buffer is consumed and the app should Enqueue more. */
typedef void (*sl_buffer_callback_t)(void *context);
int  opensl_bridge_player_set_callback(sl_player_t *player,
                                       sl_buffer_callback_t cb, void *context);

int  opensl_bridge_player_play(sl_player_t *player);     /* SetPlayState(PLAYING)   */
int  opensl_bridge_player_stop(sl_player_t *player);     /* SetPlayState(PAUSED)    */
int  opensl_bridge_player_destroy(sl_player_t *player);  /* Object::Destroy         */

/* ---- Recorder (mic input) — PARTIAL ----
 * Routes mic PCM into a ring buffer the app reads via
 * opensl_bridge_recorder_read. The current implementation installs an
 * AVAudioInputNode tap and accumulates float32 samples; opensl_bridge_recorder_read
 * drains them into the caller's buffer (no format conversion yet — caller
 * receives float32 non-interleaved at the requested sample_rate/channels). */
int  opensl_bridge_create_recorder(sl_engine_t *engine,
                                   int channels, int sample_rate, int bits_per_sample,
                                   sl_player_t **out_recorder);
int  opensl_bridge_recorder_read(sl_player_t *recorder, void *out_buf, size_t bytes);

/* AAudio stub — returns AAUDIO_ERROR_UNIMPLEMENTED for everything. */
int  aaudio_stub_unimplemented(void);

#ifdef __cplusplus
}
#endif
#endif
