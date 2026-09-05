/*
 * Per-application volume control, the same thing the Windows mixer does.
 *
 * One API, two implementations chosen at compile time: Core Audio sessions on
 * Windows, PulseAudio sink inputs elsewhere. The Pulse client also drives
 * PipeWire through its compatibility layer, which is what nearly every current
 * distribution runs, so no separate PipeWire backend is needed.
 *
 * Applications are addressed by executable basename rather than by process id,
 * because a pid changes every run while "Discord.exe" does not. Matching
 * ignores case and any extension, so one configuration works on both systems
 * where the same program is "Discord.exe" and "discord".
 *
 * Not thread safe; call from one thread.
 */

#ifndef MIXER_H
#define MIXER_H

#include <stdbool.h>

#define MIXER_NAME_MAX     64
#define MIXER_MAX_SESSIONS 64

typedef struct {
    char  process[MIXER_NAME_MAX];   /*!< executable basename */
    char  display[MIXER_NAME_MAX];   /*!< friendly name, or the basename again */
    float volume;                    /*!< 0..1 */
    bool  muted;
} mixer_session_t;

/**
 * Connect to the system mixer.
 *
 * @return false if no mixer is reachable, in which case every other call is a
 *         no-op and mixer_available() stays false.
 */
bool mixer_init(void);

void mixer_shutdown(void);

bool mixer_available(void);

/**
 * Why the mixer is unavailable, for logging. Never NULL.
 *
 * A build without PulseAudio and a session the program cannot reach both end
 * up disabled, and the two are fixed in completely different ways.
 */
const char *mixer_last_error(void);

/**
 * List the applications currently playing audio.
 *
 * @param out Receives up to @p max sessions.
 * @param max Capacity of @p out.
 * @return Number written, or 0.
 */
int mixer_enumerate(mixer_session_t *out, int max);

/**
 * Set the volume of every session belonging to @p process.
 *
 * One application often owns several sessions, so all of them are set.
 *
 * @param process Executable basename, with or without extension.
 * @param volume  0..1, clamped.
 * @return false if nothing matched.
 */
bool mixer_set_volume(const char *process, float volume);

/**
 * @param process Executable basename, with or without extension.
 * @return Volume of the first matching session, or -1 if none matched.
 */
float mixer_get_volume(const char *process);

/**
 * @param process Executable basename, with or without extension.
 * @param mute    Desired state.
 */
bool mixer_set_mute(const char *process, bool mute);

#endif /* MIXER_H */
