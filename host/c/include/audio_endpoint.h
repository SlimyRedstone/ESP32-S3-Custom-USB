/*
 * Routing an application's audio through this program, without installing a
 * driver.
 *
 * Each routed process is captured, scaled, and played back to the default
 * output as one of our own streams. The application's own session is muted so
 * only the processed copy is audible.
 *
 * Windows uses process loopback capture, available from Windows 10 2004. Linux
 * gives each routed process a null sink of its own, moves the stream onto it,
 * and reads that sink's monitor. Neither path needs a kernel driver, and
 * neither creates a device that other applications can select: this captures
 * from a named process rather than advertising an endpoint.
 *
 * This is only worth using for things volume alone cannot do, such as
 * per-application effects or sending one program to a different output. It
 * costs a buffer hop of latency, so mixer.h remains the better tool for plain
 * volume control.
 *
 * Not thread safe; call from one thread.
 */

#ifndef AUDIO_ENDPOINT_H
#define AUDIO_ENDPOINT_H

#include <stdbool.h>

#define AUDIO_ENDPOINT_NAME_MAX 64
#define AUDIO_ENDPOINT_MAX      8

/**
 * Prepare the backend.
 *
 * @return false when no audio server is reachable, after which every other call
 *         is a no-op.
 */
bool audio_endpoint_init(void);

void audio_endpoint_shutdown(void);

bool audio_endpoint_available(void);

/**
 * Begin routing a process through this program.
 *
 * @param process Executable basename, as stored in the configuration.
 * @param gain    Starting gain, 0..1.
 * @return false if the process is not playing audio, is already routed, or the
 *         backend is full.
 */
bool audio_endpoint_add(const char *process, float gain);

/** Stop routing @p process and restore its own output. */
void audio_endpoint_remove(const char *process);

/**
 * @param process Executable basename.
 * @param gain    New gain, 0..1, applied to the routed copy.
 */
void audio_endpoint_set_gain(const char *process, float gain);

/** True while @p process is being routed. */
bool audio_endpoint_is_routed(const char *process);

int audio_endpoint_count(void);

/** Reason the last call failed, for logging. Never NULL. */
const char *audio_endpoint_last_error(void);

#endif /* AUDIO_ENDPOINT_H */
