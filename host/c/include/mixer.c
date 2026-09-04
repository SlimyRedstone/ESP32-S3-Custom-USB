#include "mixer.h"

#include <stdio.h>
#include <string.h>

/* Compare basenames without case or extension, so one configuration matches
   "Discord.exe" on Windows and "discord" on Linux. */
static bool name_matches(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }

    for (;;) {
        char ca = *a;
        char cb = *b;

        bool a_end = (ca == 0 || ca == '.');
        bool b_end = (cb == 0 || cb == '.');
        if (a_end || b_end) {
            return a_end && b_end;
        }

        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) {
            return false;
        }

        a++;
        b++;
    }
}

static float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* ========================================================================== */
#ifdef _WIN32
/* ========================================================================== */

/* COBJMACROS gives the C call form; initguid.h turns the interface headers'
   DEFINE_GUID declarations into actual symbols, and must appear exactly once. */
#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

static bool s_ready;
static bool s_owns_com;

/*
 * Volume changes arrive continuously while a fader moves, and rebuilding the
 * whole session graph for each one is far too slow to keep a frame budget.
 * The per-application volume interfaces are therefore held open and only
 * rediscovered occasionally.
 */
#define CACHE_TTL_MS 2000

typedef struct {
    char name[MIXER_NAME_MAX];
    ISimpleAudioVolume *volume;
} cached_session_t;

static cached_session_t s_cache[MIXER_MAX_SESSIONS];
static int   s_cache_count;
static DWORD s_cache_stamp;
static bool  s_cache_valid;

static void cache_clear(void);

bool mixer_init(void)
{
    if (s_ready) {
        return true;
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr == S_OK || hr == S_FALSE) {
        s_owns_com = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        /* RPC_E_CHANGED_MODE only means somebody else already picked a model,
           which is fine; anything else is fatal. */
        fprintf(stderr, "mixer: CoInitializeEx failed (0x%08lx)\n", (unsigned long)hr);
        return false;
    }

    s_ready = true;
    return true;
}

void mixer_shutdown(void)
{
    cache_clear();

    if (s_owns_com) {
        CoUninitialize();
        s_owns_com = false;
    }
    s_ready = false;
}

bool mixer_available(void)
{
    return s_ready;
}

/* Executable basename for a session's owning process. */
static bool process_name_of(DWORD pid, char *out, size_t cap)
{
    out[0] = 0;

    if (pid == 0) {
        snprintf(out, cap, "System");
        return true;
    }

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == NULL) {
        return false;
    }

    char path[MAX_PATH];
    DWORD len = MAX_PATH;
    bool ok = QueryFullProcessImageNameA(proc, 0, path, &len) != 0;
    CloseHandle(proc);

    if (!ok) {
        return false;
    }

    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == 0x5C || *p == '/') {
            base = p + 1;
        }
    }
    snprintf(out, cap, "%s", base);
    return true;
}

/*
 * Walk the default render endpoint's sessions.
 *
 * @param match   Basename to act on, or NULL to visit every session.
 * @param collect Receives sessions when not NULL.
 * @param max     Capacity of @p collect.
 * @param apply   Volume to set on matches, or -1 to leave them alone.
 * @param mute    0 to unmute, 1 to mute, -1 to leave alone.
 * @param found   Receives the first matching session's volume, may be NULL.
 * @return Sessions collected, or matches acted on.
 */
static int walk_sessions(const char *match, mixer_session_t *collect, int max,
                         float apply, int mute, float *found)
{
    if (!s_ready) {
        return 0;
    }

    IMMDeviceEnumerator *devices = NULL;
    IMMDevice *endpoint = NULL;
    IAudioSessionManager2 *manager = NULL;
    IAudioSessionEnumerator *sessions = NULL;
    int hits = 0;

    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                  &IID_IMMDeviceEnumerator, (void **)&devices);
    if (FAILED(hr)) {
        return 0;
    }

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(devices, eRender, eConsole,
                                                     &endpoint);
    if (FAILED(hr)) {
        goto done;
    }

    hr = IMMDevice_Activate(endpoint, &IID_IAudioSessionManager2, CLSCTX_ALL,
                            NULL, (void **)&manager);
    if (FAILED(hr)) {
        goto done;
    }

    hr = IAudioSessionManager2_GetSessionEnumerator(manager, &sessions);
    if (FAILED(hr)) {
        goto done;
    }

    int count = 0;
    if (FAILED(IAudioSessionEnumerator_GetCount(sessions, &count))) {
        goto done;
    }

    for (int i = 0; i < count; i++) {
        IAudioSessionControl *control = NULL;
        IAudioSessionControl2 *control2 = NULL;
        ISimpleAudioVolume *volume = NULL;
        DWORD pid = 0;
        char name[MIXER_NAME_MAX];

        if (FAILED(IAudioSessionEnumerator_GetSession(sessions, i, &control))) {
            continue;
        }

        if (SUCCEEDED(IAudioSessionControl_QueryInterface(
                control, &IID_IAudioSessionControl2, (void **)&control2))) {
            IAudioSessionControl2_GetProcessId(control2, &pid);
        }

        if (!process_name_of(pid, name, sizeof(name))) {
            snprintf(name, sizeof(name), "pid %lu", (unsigned long)pid);
        }

        if (SUCCEEDED(IAudioSessionControl_QueryInterface(
                control, &IID_ISimpleAudioVolume, (void **)&volume))) {

            if (collect && hits < max) {
                float level = 0.0f;
                BOOL is_muted = FALSE;

                ISimpleAudioVolume_GetMasterVolume(volume, &level);
                ISimpleAudioVolume_GetMute(volume, &is_muted);

                snprintf(collect[hits].process, MIXER_NAME_MAX, "%s", name);
                snprintf(collect[hits].display, MIXER_NAME_MAX, "%s", name);
                collect[hits].volume = level;
                collect[hits].muted = is_muted ? true : false;
                hits++;

            } else if (match && name_matches(name, match)) {
                if (found && hits == 0) {
                    ISimpleAudioVolume_GetMasterVolume(volume, found);
                }
                if (apply >= 0.0f) {
                    ISimpleAudioVolume_SetMasterVolume(volume, apply, NULL);
                }
                if (mute >= 0) {
                    ISimpleAudioVolume_SetMute(volume, mute ? TRUE : FALSE, NULL);
                }
                hits++;
            }

            ISimpleAudioVolume_Release(volume);
        }

        if (control2) {
            IAudioSessionControl2_Release(control2);
        }
        IAudioSessionControl_Release(control);
    }

done:
    if (sessions) IAudioSessionEnumerator_Release(sessions);
    if (manager)  IAudioSessionManager2_Release(manager);
    if (endpoint) IMMDevice_Release(endpoint);
    if (devices)  IMMDeviceEnumerator_Release(devices);
    return hits;
}

static void cache_clear(void)
{
    for (int i = 0; i < s_cache_count; i++) {
        if (s_cache[i].volume) {
            ISimpleAudioVolume_Release(s_cache[i].volume);
        }
    }
    s_cache_count = 0;
    s_cache_valid = false;
}

/* Take a reference on every session's volume interface and keep it. */
static void cache_build(void)
{
    cache_clear();

    if (!s_ready) {
        return;
    }

    IMMDeviceEnumerator *devices = NULL;
    IMMDevice *endpoint = NULL;
    IAudioSessionManager2 *manager = NULL;
    IAudioSessionEnumerator *sessions = NULL;

    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator, (void **)&devices))) {
        goto done;
    }
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(devices, eRender,
                                                           eConsole, &endpoint))) {
        goto done;
    }
    if (FAILED(IMMDevice_Activate(endpoint, &IID_IAudioSessionManager2, CLSCTX_ALL,
                                  NULL, (void **)&manager))) {
        goto done;
    }
    if (FAILED(IAudioSessionManager2_GetSessionEnumerator(manager, &sessions))) {
        goto done;
    }

    int count = 0;
    if (FAILED(IAudioSessionEnumerator_GetCount(sessions, &count))) {
        goto done;
    }

    for (int i = 0; i < count && s_cache_count < MIXER_MAX_SESSIONS; i++) {
        IAudioSessionControl *control = NULL;
        IAudioSessionControl2 *control2 = NULL;
        ISimpleAudioVolume *volume = NULL;
        DWORD pid = 0;

        if (FAILED(IAudioSessionEnumerator_GetSession(sessions, i, &control))) {
            continue;
        }
        if (SUCCEEDED(IAudioSessionControl_QueryInterface(
                control, &IID_IAudioSessionControl2, (void **)&control2))) {
            IAudioSessionControl2_GetProcessId(control2, &pid);
            IAudioSessionControl2_Release(control2);
        }

        if (SUCCEEDED(IAudioSessionControl_QueryInterface(
                control, &IID_ISimpleAudioVolume, (void **)&volume))) {

            cached_session_t *entry = &s_cache[s_cache_count];
            if (!process_name_of(pid, entry->name, sizeof(entry->name))) {
                snprintf(entry->name, sizeof(entry->name), "pid %lu",
                         (unsigned long)pid);
            }
            entry->volume = volume;     /* reference kept until cache_clear */
            s_cache_count++;
        }

        IAudioSessionControl_Release(control);
    }

    s_cache_stamp = GetTickCount();
    s_cache_valid = true;

done:
    if (sessions) IAudioSessionEnumerator_Release(sessions);
    if (manager)  IAudioSessionManager2_Release(manager);
    if (endpoint) IMMDevice_Release(endpoint);
    if (devices)  IMMDeviceEnumerator_Release(devices);
}

static void cache_ensure(bool force)
{
    if (force || !s_cache_valid ||
        (GetTickCount() - s_cache_stamp) > CACHE_TTL_MS) {
        cache_build();
    }
}

int mixer_enumerate(mixer_session_t *out, int max)
{
    /* Listing is rare and wants live values, so it walks rather than caches. */
    return walk_sessions(NULL, out, max, -1.0f, -1, NULL);
}

bool mixer_set_volume(const char *process, float volume)
{
    if (!s_ready) {
        return false;
    }

    float level = clamp01(volume);

    for (int attempt = 0; attempt < 2; attempt++) {
        cache_ensure(attempt > 0);

        int hits = 0;
        bool stale = false;

        for (int i = 0; i < s_cache_count; i++) {
            if (!name_matches(s_cache[i].name, process)) {
                continue;
            }
            if (FAILED(ISimpleAudioVolume_SetMasterVolume(s_cache[i].volume,
                                                          level, NULL))) {
                stale = true;   /* the session died under us */
                break;
            }
            hits++;
        }

        if (!stale && hits > 0) {
            return true;
        }

        /* Either nothing matched or a handle went bad; rebuild once and retry
           so a newly started application is picked up promptly. */
        cache_clear();
    }
    return false;
}

float mixer_get_volume(const char *process)
{
    if (!s_ready) {
        return -1.0f;
    }

    cache_ensure(false);

    for (int i = 0; i < s_cache_count; i++) {
        if (name_matches(s_cache[i].name, process)) {
            float level = -1.0f;
            if (SUCCEEDED(ISimpleAudioVolume_GetMasterVolume(s_cache[i].volume,
                                                             &level))) {
                return level;
            }
            cache_clear();
            return -1.0f;
        }
    }
    return -1.0f;
}

bool mixer_set_mute(const char *process, bool mute)
{
    return walk_sessions(process, NULL, 0, -1.0f, mute ? 1 : 0, NULL) > 0;
}

/* ========================================================================== */
#elif defined(MIXER_HAVE_PULSE)
/* ========================================================================== */

#include <pulse/pulseaudio.h>

/*
 * A threaded mainloop is used so the caller never has to pump anything: every
 * entry point locks it, issues an operation and waits for that operation to
 * finish. That keeps the API synchronous, which is what a per-frame UI wants.
 */
static pa_threaded_mainloop *s_loop;
static pa_context *s_ctx;
static bool s_ready;

/* Scratch shared with the introspection callbacks while the loop is locked. */
static mixer_session_t *s_collect;
static int   s_collect_max;
static int   s_hits;
static const char *s_match;
static float s_apply;
static int   s_mute;
static float s_found;

static void context_state_cb(pa_context *c, void *userdata)
{
    (void)userdata;
    switch (pa_context_get_state(c)) {
    case PA_CONTEXT_READY:
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        pa_threaded_mainloop_signal(s_loop, 0);
        break;
    default:
        break;
    }
}

/* Wait for an operation, then release it. */
static void wait_for(pa_operation *op)
{
    if (op == NULL) {
        return;
    }
    while (pa_operation_get_state(op) == PA_OPERATION_RUNNING) {
        pa_threaded_mainloop_wait(s_loop);
    }
    pa_operation_unref(op);
}

static void done_cb(pa_context *c, int success, void *userdata)
{
    (void)c;
    (void)success;
    (void)userdata;
    pa_threaded_mainloop_signal(s_loop, 0);
}

/* Which executable a stream belongs to. */
static const char *binary_of(const pa_sink_input_info *info)
{
    const char *name = pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_PROCESS_BINARY);
    if (name == NULL) {
        name = pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME);
    }
    return name ? name : "unknown";
}

static void sink_input_cb(pa_context *c, const pa_sink_input_info *info,
                          int eol, void *userdata)
{
    (void)userdata;

    if (eol) {
        pa_threaded_mainloop_signal(s_loop, 0);
        return;
    }

    const char *binary = binary_of(info);
    float level = (float)pa_sw_volume_to_linear(pa_cvolume_avg(&info->volume));

    if (s_collect && s_hits < s_collect_max) {
        const char *display = pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME);

        snprintf(s_collect[s_hits].process, MIXER_NAME_MAX, "%s", binary);
        snprintf(s_collect[s_hits].display, MIXER_NAME_MAX, "%s",
                 display ? display : binary);
        s_collect[s_hits].volume = level;
        s_collect[s_hits].muted = info->mute ? true : false;
        s_hits++;
        return;
    }

    if (s_match == NULL || !name_matches(binary, s_match)) {
        return;
    }

    if (s_hits == 0) {
        s_found = level;
    }
    s_hits++;

    if (s_apply >= 0.0f) {
        pa_cvolume cv;
        pa_cvolume_set(&cv, info->volume.channels,
                       pa_sw_volume_from_linear((double)s_apply));
        pa_operation_unref(
            pa_context_set_sink_input_volume(c, info->index, &cv, NULL, NULL));
    }

    if (s_mute >= 0) {
        pa_operation_unref(
            pa_context_set_sink_input_mute(c, info->index, s_mute, NULL, NULL));
    }
}

bool mixer_init(void)
{
    if (s_ready) {
        return true;
    }

    s_loop = pa_threaded_mainloop_new();
    if (s_loop == NULL) {
        return false;
    }

    pa_mainloop_api *api = pa_threaded_mainloop_get_api(s_loop);
    s_ctx = pa_context_new(api, "IOMeeter");
    if (s_ctx == NULL) {
        pa_threaded_mainloop_free(s_loop);
        s_loop = NULL;
        return false;
    }

    pa_context_set_state_callback(s_ctx, context_state_cb, NULL);

    if (pa_context_connect(s_ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0 ||
        pa_threaded_mainloop_start(s_loop) < 0) {
        mixer_shutdown();
        return false;
    }

    pa_threaded_mainloop_lock(s_loop);
    for (;;) {
        pa_context_state_t state = pa_context_get_state(s_ctx);
        if (state == PA_CONTEXT_READY) {
            s_ready = true;
            break;
        }
        if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
            break;
        }
        pa_threaded_mainloop_wait(s_loop);
    }
    pa_threaded_mainloop_unlock(s_loop);

    if (!s_ready) {
        fprintf(stderr, "mixer: no PulseAudio or PipeWire server reachable\n");
        mixer_shutdown();
    }
    return s_ready;
}

void mixer_shutdown(void)
{
    if (s_loop) {
        pa_threaded_mainloop_stop(s_loop);
    }
    if (s_ctx) {
        pa_context_disconnect(s_ctx);
        pa_context_unref(s_ctx);
        s_ctx = NULL;
    }
    if (s_loop) {
        pa_threaded_mainloop_free(s_loop);
        s_loop = NULL;
    }
    s_ready = false;
}

bool mixer_available(void)
{
    return s_ready;
}

/* Shared entry: fills the scratch, runs one introspection pass, returns hits. */
static int run_pass(const char *match, mixer_session_t *collect, int max,
                    float apply, int mute, float *found)
{
    if (!s_ready) {
        return 0;
    }

    pa_threaded_mainloop_lock(s_loop);

    s_collect = collect;
    s_collect_max = max;
    s_hits = 0;
    s_match = match;
    s_apply = apply;
    s_mute = mute;
    s_found = -1.0f;

    wait_for(pa_context_get_sink_input_info_list(s_ctx, sink_input_cb, NULL));

    int hits = s_hits;
    if (found) {
        *found = s_found;
    }
    s_collect = NULL;
    s_match = NULL;

    pa_threaded_mainloop_unlock(s_loop);
    return hits;
}

int mixer_enumerate(mixer_session_t *out, int max)
{
    return run_pass(NULL, out, max, -1.0f, -1, NULL);
}

bool mixer_set_volume(const char *process, float volume)
{
    return run_pass(process, NULL, 0, clamp01(volume), -1, NULL) > 0;
}

float mixer_get_volume(const char *process)
{
    float level = -1.0f;
    if (run_pass(process, NULL, 0, -1.0f, -1, &level) == 0) {
        return -1.0f;
    }
    return level;
}

bool mixer_set_mute(const char *process, bool mute)
{
    return run_pass(process, NULL, 0, -1.0f, mute ? 1 : 0, NULL) > 0;
}

/* ========================================================================== */
#else
/* ========================================================================== */

/*
 * Built without libpulse. Everything reports unavailable rather than failing,
 * so the interface still runs; only volume control is missing.
 */

bool mixer_init(void)
{
    fprintf(stderr, "mixer: built without PulseAudio support\n");
    return false;
}

void mixer_shutdown(void) {}
bool mixer_available(void) { return false; }

int mixer_enumerate(mixer_session_t *out, int max)
{
    (void)out;
    (void)max;
    return 0;
}

bool mixer_set_volume(const char *process, float volume)
{
    (void)process;
    (void)volume;
    return false;
}

float mixer_get_volume(const char *process)
{
    (void)process;
    return -1.0f;
}

bool mixer_set_mute(const char *process, bool mute)
{
    (void)process;
    (void)mute;
    return false;
}

#endif
