#include "audio_endpoint.h"

#include <stdio.h>
#include <string.h>

/* Everything runs at this format; both backends convert to it. */
#define AE_RATE     48000
#define AE_CHANNELS 2

static char s_error[160] = "";

static void set_error(const char *text)
{
    snprintf(s_error, sizeof(s_error), "%s", text ? text : "");
}

const char *audio_endpoint_last_error(void)
{
    return s_error;
}

static float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* Compare basenames ignoring case and extension, as mixer.c does. */
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

/* ========================================================================== */
#ifdef _WIN32
/* ========================================================================== */

#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <objidl.h>
#include <tlhelp32.h>

/*
 * audioclientactivationparams.h is absent from mingw-w64, so the process
 * loopback definitions are reproduced here from the documented layout.
 */
typedef enum {
    AE_ACTIVATION_TYPE_DEFAULT = 0,
    AE_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1,
} AE_ACTIVATION_TYPE;

typedef enum {
    AE_LOOPBACK_INCLUDE_TREE = 0,
    AE_LOOPBACK_EXCLUDE_TREE = 1,
} AE_LOOPBACK_MODE;

typedef struct {
    DWORD            TargetProcessId;
    AE_LOOPBACK_MODE ProcessLoopbackMode;
} AE_PROCESS_LOOPBACK_PARAMS;

typedef struct {
    AE_ACTIVATION_TYPE ActivationType;
    union {
        AE_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
    } u;
} AE_ACTIVATION_PARAMS;

/* The audio client codes that actually come up in practice. */
static const char *audclnt_reason(long hr)
{
    switch ((unsigned long)hr) {
    case 0x8889000AUL:  /* AUDCLNT_E_DEVICE_IN_USE */
        return "the output device is held in exclusive mode "
               "(VoiceMeeter, ASIO or an exclusive-mode player)";
    case 0x88890004UL:  /* AUDCLNT_E_DEVICE_INVALIDATED */
        return "the output device went away";
    case 0x88890008UL:  /* AUDCLNT_E_UNSUPPORTED_FORMAT */
        return "the device rejected the format";
    case 0x8889000EUL:  /* AUDCLNT_E_SERVICE_NOT_RUNNING */
        return "the Windows Audio service is not running";
    case 0x80070005UL:  /* E_ACCESSDENIED */
        return "access denied";
    default:
        return NULL;
    }
}

static void set_error_hr(const char *what, long hr)
{
    char buf[160];
    const char *why = audclnt_reason(hr);

    if (why) {
        snprintf(buf, sizeof(buf), "%s: %s", what, why);
    } else {
        snprintf(buf, sizeof(buf), "%s failed (0x%08lx)", what, (unsigned long)hr);
    }
    set_error(buf);
}

/* The pseudo-device name that routes activation to the loopback provider. */
static const WCHAR AE_LOOPBACK_DEVICE[] = L"VAD\\Process_Loopback";

typedef struct {
    bool  used;
    char  process[AUDIO_ENDPOINT_NAME_MAX];
    DWORD pid;

    IAudioClient        *capture;
    IAudioCaptureClient *capture_reader;
    IAudioClient        *render;
    IAudioRenderClient  *render_writer;

    WORD    channels;
    HANDLE  ready;      /* signalled by the capture client */
    HANDLE  thread;
    HANDLE  stop;
    volatile LONG gain_milli;   /* gain * 1000, read by the worker */
} ae_stream_t;

static ae_stream_t s_streams[AUDIO_ENDPOINT_MAX];
static bool s_ready;
static bool s_owns_com;

/* ---- completion handler -------------------------------------------------- */

/*
 * ActivateAudioInterfaceAsync answers through a COM callback, so the interface
 * is implemented here by hand: a vtable plus an object whose first member is
 * the interface pointer.
 */
typedef struct {
    IActivateAudioInterfaceCompletionHandler iface;
    LONG   refs;
    HANDLE done;
} ae_handler_t;

static HRESULT STDMETHODCALLTYPE handler_query(
        IActivateAudioInterfaceCompletionHandler *self, REFIID riid, void **out)
{
    /*
     * IAgileObject must be answered here. Without it COM cannot marshal the
     * callback across apartments and ActivateAudioInterfaceAsync refuses with
     * E_ILLEGAL_METHOD_CALL. It is a marker interface, so returning this object
     * is the whole implementation; Microsoft's own sample gets the same effect
     * by deriving its handler from FtmBase.
     */
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_IAgileObject) ||
        IsEqualIID(riid, &IID_IActivateAudioInterfaceCompletionHandler)) {
        *out = self;
        self->lpVtbl->AddRef(self);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE handler_addref(
        IActivateAudioInterfaceCompletionHandler *self)
{
    ae_handler_t *h = (ae_handler_t *)self;
    return (ULONG)InterlockedIncrement(&h->refs);
}

static ULONG STDMETHODCALLTYPE handler_release(
        IActivateAudioInterfaceCompletionHandler *self)
{
    ae_handler_t *h = (ae_handler_t *)self;
    LONG n = InterlockedDecrement(&h->refs);
    return (ULONG)n;
}

static HRESULT STDMETHODCALLTYPE handler_completed(
        IActivateAudioInterfaceCompletionHandler *self,
        IActivateAudioInterfaceAsyncOperation *op)
{
    ae_handler_t *h = (ae_handler_t *)self;
    (void)op;
    SetEvent(h->done);
    return S_OK;
}

static CONST_VTBL IActivateAudioInterfaceCompletionHandlerVtbl s_handler_vtbl = {
    handler_query,
    handler_addref,
    handler_release,
    handler_completed,
};

/* ---- helpers ------------------------------------------------------------- */

/* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, spelled out because pulling in ksmedia.h
   with INITGUID clashes with the definitions already made in mixer.c. */
static const GUID AE_SUBTYPE_FLOAT = {
    0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};

/* The engine mixes in 32-bit float, either plainly tagged or as extensible. */
static bool format_is_float(const WAVEFORMATEX *wf)
{
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return true;
    }
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        wf->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)wf;
        return IsEqualGUID(&ext->SubFormat, &AE_SUBTYPE_FLOAT);
    }
    return false;
}

static DWORD pid_of_process(const char *process)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32 entry;
    entry.dwSize = sizeof(entry);
    DWORD found = 0;

    if (Process32First(snap, &entry)) {
        do {
            if (name_matches(entry.szExeFile, process)) {
                found = entry.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &entry));
    }

    CloseHandle(snap);
    return found;
}

/* Activate a process-loopback capture client for one pid. */
static IAudioClient *activate_loopback(DWORD pid)
{
    AE_ACTIVATION_PARAMS params;
    ZeroMemory(&params, sizeof(params));
    params.ActivationType = AE_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.u.ProcessLoopbackParams.TargetProcessId = pid;
    params.u.ProcessLoopbackParams.ProcessLoopbackMode = AE_LOOPBACK_INCLUDE_TREE;

    PROPVARIANT prop;
    PropVariantInit(&prop);
    prop.vt = VT_BLOB;
    prop.blob.cbSize = sizeof(params);
    prop.blob.pBlobData = (BYTE *)&params;

    ae_handler_t handler;
    handler.iface.lpVtbl = &s_handler_vtbl;
    handler.refs = 1;
    handler.done = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (handler.done == NULL) {
        return NULL;
    }

    IActivateAudioInterfaceAsyncOperation *op = NULL;
    HRESULT hr = ActivateAudioInterfaceAsync(AE_LOOPBACK_DEVICE,
                                             &IID_IAudioClient, &prop,
                                             &handler.iface, &op);
    if (FAILED(hr)) {
        CloseHandle(handler.done);
        set_error_hr("ActivateAudioInterfaceAsync", hr);
        return NULL;
    }

    WaitForSingleObject(handler.done, 2000);
    CloseHandle(handler.done);

    IAudioClient *client = NULL;
    HRESULT activate_hr = E_FAIL;

    if (op) {
        IUnknown *unknown = NULL;
        if (SUCCEEDED(IActivateAudioInterfaceAsyncOperation_GetActivateResult(
                op, &activate_hr, &unknown)) && SUCCEEDED(activate_hr) && unknown) {
            IUnknown_QueryInterface(unknown, &IID_IAudioClient, (void **)&client);
            IUnknown_Release(unknown);
        }
        IActivateAudioInterfaceAsyncOperation_Release(op);
    }

    if (client == NULL) {
        set_error_hr("process loopback activation", activate_hr);
    }
    return client;
}

/* Pull captured frames, scale them, and push them to the output. */
static DWORD WINAPI stream_worker(LPVOID arg)
{
    ae_stream_t *s = (ae_stream_t *)arg;
    HANDLE waits[2] = { s->stop, s->ready };

    for (;;) {
        DWORD hit = WaitForMultipleObjects(2, waits, FALSE, 200);
        if (hit == WAIT_OBJECT_0) {
            break;
        }

        for (;;) {
            BYTE *data = NULL;
            UINT32 frames = 0;
            DWORD flags = 0;

            HRESULT hr = IAudioCaptureClient_GetBuffer(s->capture_reader, &data,
                                                       &frames, &flags, NULL, NULL);
            if (hr != S_OK || frames == 0) {
                if (hr == S_OK) {
                    IAudioCaptureClient_ReleaseBuffer(s->capture_reader, frames);
                }
                break;
            }

            UINT32 padding = 0, buffer = 0;
            if (SUCCEEDED(IAudioClient_GetBufferSize(s->render, &buffer)) &&
                SUCCEEDED(IAudioClient_GetCurrentPadding(s->render, &padding))) {

                UINT32 room = buffer - padding;
                UINT32 write = (frames < room) ? frames : room;
                BYTE *out = NULL;

                if (write > 0 &&
                    SUCCEEDED(IAudioRenderClient_GetBuffer(s->render_writer,
                                                           write, &out))) {
                    float gain = (float)InterlockedCompareExchange(
                                     &s->gain_milli, 0, 0) / 1000.0f;

                    const float *src = (const float *)data;
                    float *dst = (float *)out;
                    UINT32 samples = write * s->channels;

                    /* A silent block still has to be written, or the render
                       stream starves and clicks. */
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        memset(dst, 0, samples * sizeof(float));
                    } else {
                        for (UINT32 i = 0; i < samples; i++) {
                            dst[i] = src[i] * gain;
                        }
                    }

                    IAudioRenderClient_ReleaseBuffer(s->render_writer, write, 0);
                }
            }

            IAudioCaptureClient_ReleaseBuffer(s->capture_reader, frames);
        }
    }
    return 0;
}

static void stream_close(ae_stream_t *s)
{
    if (s->thread) {
        SetEvent(s->stop);
        WaitForSingleObject(s->thread, 1000);
        CloseHandle(s->thread);
        s->thread = NULL;
    }
    if (s->capture)        IAudioClient_Stop(s->capture);
    if (s->render)         IAudioClient_Stop(s->render);
    if (s->capture_reader) IAudioCaptureClient_Release(s->capture_reader);
    if (s->render_writer)  IAudioRenderClient_Release(s->render_writer);
    if (s->capture)        IAudioClient_Release(s->capture);
    if (s->render)         IAudioClient_Release(s->render);
    if (s->ready)          CloseHandle(s->ready);
    if (s->stop)           CloseHandle(s->stop);

    ZeroMemory(s, sizeof(*s));
}

/*
 * Open playback on the default endpoint and report the format it chose.
 *
 * Shared mode only accepts the endpoint's own mix format, so that format is
 * read here and handed back for the capture side to reuse. Using one format for
 * both ends removes any conversion between them.
 *
 * The caller frees *format with CoTaskMemFree.
 */
static bool open_render(ae_stream_t *s, WAVEFORMATEX **format)
{
    IMMDeviceEnumerator *devices = NULL;
    IMMDevice *endpoint = NULL;
    WAVEFORMATEX *mix = NULL;
    bool ok = false;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&devices);
    if (FAILED(hr)) {
        set_error_hr("CoCreateInstance(MMDeviceEnumerator)", hr);
        goto done;
    }

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(devices, eRender, eConsole,
                                                     &endpoint);
    if (FAILED(hr)) {
        set_error_hr("GetDefaultAudioEndpoint", hr);
        goto done;
    }

    hr = IMMDevice_Activate(endpoint, &IID_IAudioClient, CLSCTX_ALL, NULL,
                            (void **)&s->render);
    if (FAILED(hr)) {
        set_error_hr("Activate(IAudioClient)", hr);
        goto done;
    }

    hr = IAudioClient_GetMixFormat(s->render, &mix);
    if (FAILED(hr)) {
        set_error_hr("GetMixFormat", hr);
        goto done;
    }

    /* The gain stage works on 32-bit floats, which is what the engine mixes in;
       anything else would need a conversion pass that does not exist here. */
    if (!format_is_float(mix)) {
        set_error("default output is not 32-bit float; unsupported");
        goto done;
    }

    hr = IAudioClient_Initialize(s->render, AUDCLNT_SHAREMODE_SHARED, 0,
                                 10 * 1000 * 10, 0, mix, NULL);
    if (FAILED(hr)) {
        set_error_hr("render Initialize", hr);
        goto done;
    }

    hr = IAudioClient_GetService(s->render, &IID_IAudioRenderClient,
                                 (void **)&s->render_writer);
    if (FAILED(hr)) {
        set_error_hr("GetService(IAudioRenderClient)", hr);
        goto done;
    }

    *format = mix;
    mix = NULL;
    ok = true;

done:
    if (mix)      CoTaskMemFree(mix);
    if (endpoint) IMMDevice_Release(endpoint);
    if (devices)  IMMDeviceEnumerator_Release(devices);
    return ok;
}

bool audio_endpoint_init(void)
{
    if (s_ready) {
        return true;
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (hr == S_OK || hr == S_FALSE) {
        s_owns_com = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        set_error("CoInitializeEx failed");
        return false;
    }

    s_ready = true;
    return true;
}

void audio_endpoint_shutdown(void)
{
    for (int i = 0; i < AUDIO_ENDPOINT_MAX; i++) {
        if (s_streams[i].used) {
            stream_close(&s_streams[i]);
        }
    }
    if (s_owns_com) {
        CoUninitialize();
        s_owns_com = false;
    }
    s_ready = false;
}

bool audio_endpoint_available(void)
{
    return s_ready;
}

bool audio_endpoint_add(const char *process, float gain)
{
    if (!s_ready || process == NULL) {
        set_error("endpoint not started");
        return false;
    }
    if (audio_endpoint_is_routed(process)) {
        set_error("already routed");
        return false;
    }

    ae_stream_t *s = NULL;
    for (int i = 0; i < AUDIO_ENDPOINT_MAX; i++) {
        if (!s_streams[i].used) {
            s = &s_streams[i];
            break;
        }
    }
    if (s == NULL) {
        set_error("no free routing slot");
        return false;
    }

    DWORD pid = pid_of_process(process);
    if (pid == 0) {
        set_error("process is not running");
        return false;
    }

    ZeroMemory(s, sizeof(*s));
    s->pid = pid;
    snprintf(s->process, sizeof(s->process), "%s", process);
    s->gain_milli = (LONG)(clamp01(gain) * 1000.0f);

    /* Playback is opened first: it decides the format both ends will use. */
    WAVEFORMATEX *wf = NULL;
    if (!open_render(s, &wf)) {
        stream_close(s);
        return false;
    }

    s->channels = wf->nChannels;

    s->capture = activate_loopback(pid);
    if (s->capture == NULL) {
        CoTaskMemFree(wf);
        stream_close(s);
        return false;
    }

    /*
     * Loopback capture must be event driven and given an explicit format:
     * GetMixFormat is not supported on the loopback pseudo-device.
     */
    HRESULT init_hr = IAudioClient_Initialize(s->capture, AUDCLNT_SHAREMODE_SHARED,
                                              AUDCLNT_STREAMFLAGS_LOOPBACK |
                                              AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                              10 * 1000 * 10, 0, wf, NULL);
    CoTaskMemFree(wf);

    if (FAILED(init_hr)) {
        set_error_hr("loopback Initialize", init_hr);
        stream_close(s);
        return false;
    }

    s->ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    s->stop = CreateEventW(NULL, TRUE, FALSE, NULL);

    if (s->ready == NULL || s->stop == NULL) {
        set_error("could not create the stream events");
        stream_close(s);
        return false;
    }

    HRESULT hr = IAudioClient_SetEventHandle(s->capture, s->ready);
    if (FAILED(hr)) {
        set_error_hr("SetEventHandle", hr);
        stream_close(s);
        return false;
    }

    hr = IAudioClient_GetService(s->capture, &IID_IAudioCaptureClient,
                                 (void **)&s->capture_reader);
    if (FAILED(hr)) {
        set_error_hr("GetService(IAudioCaptureClient)", hr);
        stream_close(s);
        return false;
    }

    if (FAILED(IAudioClient_Start(s->capture)) ||
        FAILED(IAudioClient_Start(s->render))) {
        set_error("could not start the streams");
        stream_close(s);
        return false;
    }

    s->used = true;
    s->thread = CreateThread(NULL, 0, stream_worker, s, 0, NULL);
    if (s->thread == NULL) {
        set_error("could not start the worker");
        stream_close(s);
        return false;
    }

    set_error("");
    return true;
}

void audio_endpoint_remove(const char *process)
{
    for (int i = 0; i < AUDIO_ENDPOINT_MAX; i++) {
        if (s_streams[i].used && name_matches(s_streams[i].process, process)) {
            stream_close(&s_streams[i]);
            return;
        }
    }
}

void audio_endpoint_set_gain(const char *process, float gain)
{
    for (int i = 0; i < AUDIO_ENDPOINT_MAX; i++) {
        if (s_streams[i].used && name_matches(s_streams[i].process, process)) {
            InterlockedExchange(&s_streams[i].gain_milli,
                                (LONG)(clamp01(gain) * 1000.0f));
            return;
        }
    }
}

/* ========================================================================== */
#elif defined(MIXER_HAVE_PULSE)
/* ========================================================================== */

#include <pulse/pulseaudio.h>
#include <stdlib.h>

/*
 * Each routed process gets a null sink of its own. Its stream is moved there,
 * the sink's monitor is recorded, scaled, and written back to the real output.
 * One shared null sink would be simpler but would mix the applications together
 * before the gain stage, losing per-application control.
 */
typedef struct {
    bool  used;
    char  process[AUDIO_ENDPOINT_NAME_MAX];
    char  sink_name[64];
    uint32_t module;        /* null sink module index, for unloading */
    pa_stream *record;
    pa_stream *play;
    float gain;
} ae_stream_t;

static ae_stream_t s_streams[AUDIO_ENDPOINT_MAX];
static pa_threaded_mainloop *s_loop;
static pa_context *s_ctx;
static bool s_ready;

/* Scratch shared with the introspection callbacks while the loop is locked. */
static uint32_t s_module_result;
static uint32_t s_found_index;
static const char *s_wanted;

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

static void module_cb(pa_context *c, uint32_t index, void *userdata)
{
    (void)c;
    (void)userdata;
    s_module_result = index;
    pa_threaded_mainloop_signal(s_loop, 0);
}

static void success_cb(pa_context *c, int success, void *userdata)
{
    (void)c;
    (void)success;
    (void)userdata;
    pa_threaded_mainloop_signal(s_loop, 0);
}

/* Find the sink input belonging to the wanted executable. */
static void find_input_cb(pa_context *c, const pa_sink_input_info *info,
                          int eol, void *userdata)
{
    (void)c;
    (void)userdata;

    if (eol) {
        pa_threaded_mainloop_signal(s_loop, 0);
        return;
    }

    const char *binary = pa_proplist_gets(info->proplist,
                                          PA_PROP_APPLICATION_PROCESS_BINARY);
    if (binary == NULL) {
        binary = pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME);
    }

    if (s_found_index == PA_INVALID_INDEX && binary &&
        name_matches(binary, s_wanted)) {
        s_found_index = info->index;
    }
}

/* Move captured audio through the gain stage and on to the output. */
static void record_cb(pa_stream *stream, size_t bytes, void *userdata)
{
    ae_stream_t *s = (ae_stream_t *)userdata;
    const void *data = NULL;

    (void)bytes;

    while (pa_stream_readable_size(stream) > 0) {
        size_t length = 0;
        if (pa_stream_peek(stream, &data, &length) < 0 || length == 0) {
            break;
        }

        /* A hole reported by the server has data == NULL; skip it rather than
           writing whatever the pointer happens to be. */
        if (data && s->play) {
            size_t samples = length / sizeof(float);
            float *scaled = malloc(length);

            if (scaled) {
                const float *src = (const float *)data;
                for (size_t i = 0; i < samples; i++) {
                    scaled[i] = src[i] * s->gain;
                }
                pa_stream_write(s->play, scaled, length, NULL, 0, PA_SEEK_RELATIVE);
                free(scaled);
            }
        }

        pa_stream_drop(stream);
    }
}

bool audio_endpoint_init(void)
{
    if (s_ready) {
        return true;
    }

    s_loop = pa_threaded_mainloop_new();
    if (s_loop == NULL) {
        set_error("could not create the mainloop");
        return false;
    }

    s_ctx = pa_context_new(pa_threaded_mainloop_get_api(s_loop),
                           "IOMeeter endpoint");
    if (s_ctx == NULL) {
        pa_threaded_mainloop_free(s_loop);
        s_loop = NULL;
        set_error("could not create the context");
        return false;
    }

    pa_context_set_state_callback(s_ctx, context_state_cb, NULL);

    if (pa_context_connect(s_ctx, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0 ||
        pa_threaded_mainloop_start(s_loop) < 0) {
        audio_endpoint_shutdown();
        set_error("could not connect to the audio server");
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
        set_error("no PulseAudio or PipeWire server reachable");
        audio_endpoint_shutdown();
    }
    return s_ready;
}

void audio_endpoint_shutdown(void)
{
    for (int i = 0; i < AUDIO_ENDPOINT_MAX; i++) {
        if (s_streams[i].used) {
            audio_endpoint_remove(s_streams[i].process);
        }
    }

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

bool audio_endpoint_available(void)
{
    return s_ready;
}

bool audio_endpoint_add(const char *process, float gain)
{
    if (!s_ready || process == NULL) {
        set_error("endpoint not started");
        return false;
    }
    if (audio_endpoint_is_routed(process)) {
        set_error("already routed");
        return false;
    }

    ae_stream_t *s = NULL;
    for (int i = 0; i < AUDIO_ENDPOINT_MAX; i++) {
        if (!s_streams[i].used) {
            s = &s_streams[i];
            break;
        }
    }
    if (s == NULL) {
        set_error("no free routing slot");
        return false;
    }

    memset(s, 0, sizeof(*s));
    snprintf(s->process, sizeof(s->process), "%s", process);
    snprintf(s->sink_name, sizeof(s->sink_name), "iometer_%s", process);

    /* Sink names accept a narrow character set; keep it to something safe. */
    for (char *p = s->sink_name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) {
            *p = '_';
        }
    }

    s->gain = clamp01(gain);

    pa_sample_spec spec;
    spec.format = PA_SAMPLE_FLOAT32NE;
    spec.rate = AE_RATE;
    spec.channels = AE_CHANNELS;

    pa_threaded_mainloop_lock(s_loop);

    char args[256];
    snprintf(args, sizeof(args),
             "sink_name=%s sink_properties=device.description=%s",
             s->sink_name, s->sink_name);

    s_module_result = PA_INVALID_INDEX;
    wait_for(pa_context_load_module(s_ctx, "module-null-sink", args,
                                    module_cb, NULL));
    s->module = s_module_result;

    if (s->module == PA_INVALID_INDEX) {
        pa_threaded_mainloop_unlock(s_loop);
        set_error("could not create the routing sink");
        memset(s, 0, sizeof(*s));
        return false;
    }

    s_found_index = PA_INVALID_INDEX;
    s_wanted = s->process;
    wait_for(pa_context_get_sink_input_info_list(s_ctx, find_input_cb, NULL));

    if (s_found_index == PA_INVALID_INDEX) {
        wait_for(pa_context_unload_module(s_ctx, s->module, success_cb, NULL));
        pa_threaded_mainloop_unlock(s_loop);
        set_error("process is not playing audio");
        memset(s, 0, sizeof(*s));
        return false;
    }

    wait_for(pa_context_move_sink_input_by_name(s_ctx, s_found_index,
                                                s->sink_name, success_cb, NULL));

    char monitor[96];
    snprintf(monitor, sizeof(monitor), "%s.monitor", s->sink_name);

    s->record = pa_stream_new(s_ctx, "endpoint capture", &spec, NULL);
    s->play = pa_stream_new(s_ctx, "endpoint playback", &spec, NULL);

    if (s->record == NULL || s->play == NULL) {
        pa_threaded_mainloop_unlock(s_loop);
        set_error("could not create the streams");
        audio_endpoint_remove(s->process);
        return false;
    }

    pa_stream_set_read_callback(s->record, record_cb, s);
    pa_stream_connect_record(s->record, monitor, NULL, PA_STREAM_ADJUST_LATENCY);
    pa_stream_connect_playback(s->play, NULL, NULL, PA_STREAM_ADJUST_LATENCY,
                               NULL, NULL);

    s->used = true;
    pa_threaded_mainloop_unlock(s_loop);

    set_error("");
    return true;
}

void audio_endpoint_remove(const char *process)
{
    for (int i = 0; i < AUDIO_ENDPOINT_MAX; i++) {
        ae_stream_t *s = &s_streams[i];
        if (!s->used || !name_matches(s->process, process)) {
            continue;
        }

        pa_threaded_mainloop_lock(s_loop);

        if (s->record) {
            pa_stream_disconnect(s->record);
            pa_stream_unref(s->record);
        }
        if (s->play) {
            pa_stream_disconnect(s->play);
            pa_stream_unref(s->play);
        }

        /* Unloading the null sink returns the application to its old output. */
        if (s->module != PA_INVALID_INDEX) {
            wait_for(pa_context_unload_module(s_ctx, s->module, success_cb, NULL));
        }

        pa_threaded_mainloop_unlock(s_loop);
        memset(s, 0, sizeof(*s));
        return;
    }
}

void audio_endpoint_set_gain(const char *process, float gain)
{
    for (int i = 0; i < AUDIO_ENDPOINT_MAX; i++) {
        if (s_streams[i].used && name_matches(s_streams[i].process, process)) {
            s_streams[i].gain = clamp01(gain);
            return;
        }
    }
}

/* ========================================================================== */
#else
/* ========================================================================== */

bool audio_endpoint_init(void)
{
    set_error("built without PulseAudio support");
    return false;
}

void audio_endpoint_shutdown(void) {}
bool audio_endpoint_available(void) { return false; }

bool audio_endpoint_add(const char *process, float gain)
{
    (void)process;
    (void)gain;
    set_error("built without PulseAudio support");
    return false;
}

void audio_endpoint_remove(const char *process) { (void)process; }

void audio_endpoint_set_gain(const char *process, float gain)
{
    (void)process;
    (void)gain;
}

#endif

/* ---- shared across the backends ----------------------------------------- */

bool audio_endpoint_is_routed(const char *process)
{
#if defined(_WIN32) || defined(MIXER_HAVE_PULSE)
    for (int i = 0; i < AUDIO_ENDPOINT_MAX; i++) {
        if (s_streams[i].used && name_matches(s_streams[i].process, process)) {
            return true;
        }
    }
#else
    (void)process;
#endif
    return false;
}

int audio_endpoint_count(void)
{
#if defined(_WIN32) || defined(MIXER_HAVE_PULSE)
    int n = 0;
    for (int i = 0; i < AUDIO_ENDPOINT_MAX; i++) {
        if (s_streams[i].used) {
            n++;
        }
    }
    return n;
#else
    return 0;
#endif
}
