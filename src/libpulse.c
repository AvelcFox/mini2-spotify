#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <math.h>

#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031
#endif

#define LOG(fmt, ...) do { \
    FILE *lf = fopen("/tmp/pulse_debug.log", "a"); \
    if (lf) { \
        fprintf(lf, "[libpulse] " fmt "\n", ##__VA_ARGS__); \
        fclose(lf); \
    } \
} while(0)

/* PulseAudio Enums */
typedef enum pa_context_state {
    PA_CONTEXT_UNCONNECTED = 0,
    PA_CONTEXT_CONNECTING = 1,
    PA_CONTEXT_AUTHORIZING = 2,
    PA_CONTEXT_SETTING_NAME = 3,
    PA_CONTEXT_READY = 4,
    PA_CONTEXT_FAILED = 5,
    PA_CONTEXT_TERMINATED = 6
} pa_context_state_t;

typedef enum pa_stream_state {
    PA_STREAM_UNCONNECTED = 0,
    PA_STREAM_CREATING = 1,
    PA_STREAM_READY = 2,
    PA_STREAM_FAILED = 3,
    PA_STREAM_TERMINATED = 4
} pa_stream_state_t;

typedef enum pa_operation_state {
    PA_OPERATION_RUNNING = 0,
    PA_OPERATION_DONE = 1,
    PA_OPERATION_CANCELLED = 2
} pa_operation_state_t;

typedef enum pa_sample_format {
    PA_SAMPLE_U8 = 0,
    PA_SAMPLE_ALAW = 1,
    PA_SAMPLE_ULAW = 2,
    PA_SAMPLE_S16LE = 3,
    PA_SAMPLE_S16BE = 4,
    PA_SAMPLE_FLOAT32LE = 5,
    PA_SAMPLE_FLOAT32BE = 6,
    PA_SAMPLE_S32LE = 7,
    PA_SAMPLE_S32BE = 8,
    PA_SAMPLE_S24LE = 9,
    PA_SAMPLE_S24BE = 10,
    PA_SAMPLE_S24_32LE = 11,
    PA_SAMPLE_S24_32BE = 12,
    PA_SAMPLE_MAX = 13,
    PA_SAMPLE_INVALID = -1
} pa_sample_format_t;

typedef uint64_t pa_usec_t;

typedef struct pa_sample_spec {
    pa_sample_format_t format;
    uint32_t rate;
    uint8_t channels;
} pa_sample_spec;

typedef struct pa_buffer_attr {
    uint32_t maxlength;
    uint32_t tlength;
    uint32_t prebuf;
    uint32_t minreq;
    uint32_t fragsize;
} pa_buffer_attr;

typedef struct pa_timing_info {
    struct timeval timestamp;
    int synchronized_clocks;
    pa_usec_t sink_usec;
    pa_usec_t source_usec;
    pa_usec_t transport_usec;
    int playing;
    int write_index_corrupt;
    int64_t write_index;
    int read_index_corrupt;
    int64_t read_index;
    pa_usec_t since_underrun;
} pa_timing_info;

typedef struct pa_cvolume {
    uint8_t channels;
    uint32_t values[32];
} pa_cvolume;

typedef struct pa_threaded_mainloop {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    volatile int running;
} pa_threaded_mainloop;

typedef struct pa_context {
    pa_context_state_t state;
    pa_threaded_mainloop *mainloop;
    void (*state_callback)(struct pa_context *c, void *userdata);
    void *state_userdata;
    void (*subscribe_callback)(struct pa_context *c, int event, uint32_t idx, void *userdata);
    void *subscribe_userdata;
} pa_context;

typedef struct pa_stream {
    pa_stream_state_t state;
    pa_context *context;
    pa_sample_spec ss;
    pa_buffer_attr attr;
    int corked;
    FILE *pcm_pipe;
    pid_t aplay_pid;
    uint64_t bytes_written;
    uint64_t read_bytes_at_cork;
    struct timeval uncork_time;
    int started_notified;
    pa_timing_info timing;
    uint8_t *prebuf;
    size_t prebuf_len;
    size_t prebuf_cap;
    void (*state_callback)(struct pa_stream *s, void *userdata);
    void *state_userdata;
    void (*started_callback)(struct pa_stream *s, void *userdata);
    void *started_userdata;
    void (*underflow_callback)(struct pa_stream *s, void *userdata);
    void *underflow_userdata;
} pa_stream;

typedef struct pa_operation {
    pa_operation_state_t state;
} pa_operation;

typedef struct pa_sink_input_info {
    uint32_t index;
    const char *name;
    uint32_t owner_module;
    uint32_t client;
    uint32_t sink;
    pa_sample_spec sample_spec;
    void *channel_map;
    pa_cvolume volume;
    void *buffer_usec;
    void *sink_usec;
    const char *resample_method;
    const char *driver;
    int mute;
    void *proplist;
    int corked;
    int has_volume;
    int volume_writable;
    void *format;
} pa_sink_input_info;

typedef void (*pa_stream_success_cb_t)(pa_stream *s, int success, void *userdata);
typedef void (*pa_context_success_cb_t)(pa_context *c, int success, void *userdata);
typedef void (*pa_sink_input_info_cb_t)(pa_context *c, const pa_sink_input_info *i, int eol, void *userdata);

static inline uint64_t get_bps(const pa_stream *s) {
    if (!s) return 352800ULL;
    uint32_t rate = s->ss.rate ? s->ss.rate : 44100;
    uint32_t ch = s->ss.channels ? s->ss.channels : 2;
    uint32_t sample_size = (s->ss.format == PA_SAMPLE_FLOAT32LE || s->ss.format == PA_SAMPLE_S32LE) ? 4 : 2;
    return (uint64_t)rate * ch * sample_size;
}

static void check_and_notify_started(pa_stream *s) {
    if (s && s->started_callback && !s->started_notified && !s->corked && s->bytes_written >= 4096) {
        s->started_notified = 1;
        LOG("🌟 [libpulse] TRIGGERING started_callback! bytes_written=%llu", (unsigned long long)s->bytes_written);
        s->started_callback(s, s->started_userdata);
    }
}

/* Mainloop background thread */
static void *mainloop_worker(void *arg) {
    pa_threaded_mainloop *m = (pa_threaded_mainloop *)arg;
    while (m->running) {
        usleep(20000); // 20ms
        pthread_mutex_lock(&m->mutex);
        pthread_cond_broadcast(&m->cond);
        pthread_mutex_unlock(&m->mutex);
    }
    return NULL;
}

/* Threaded Mainloop API */
pa_threaded_mainloop *pa_threaded_mainloop_new(void) {
    LOG("pa_threaded_mainloop_new");
    pa_threaded_mainloop *m = calloc(1, sizeof(pa_threaded_mainloop));
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    pthread_cond_init(&m->cond, NULL);
    return m;
}

void pa_threaded_mainloop_free(pa_threaded_mainloop *m) {
    LOG("pa_threaded_mainloop_free");
    if (!m) return;
    if (m->running) {
        m->running = 0;
        pthread_join(m->thread, NULL);
    }
    pthread_mutex_destroy(&m->mutex);
    pthread_cond_destroy(&m->cond);
    free(m);
}

int pa_threaded_mainloop_start(pa_threaded_mainloop *m) {
    LOG("pa_threaded_mainloop_start");
    if (!m) return -1;
    m->running = 1;
    pthread_create(&m->thread, NULL, mainloop_worker, m);
    return 0;
}

void pa_threaded_mainloop_stop(pa_threaded_mainloop *m) {
    LOG("pa_threaded_mainloop_stop");
    if (!m) return;
    m->running = 0;
    pthread_cond_broadcast(&m->cond);
    pthread_join(m->thread, NULL);
}

void pa_threaded_mainloop_lock(pa_threaded_mainloop *m) {
    if (m) pthread_mutex_lock(&m->mutex);
}

void pa_threaded_mainloop_unlock(pa_threaded_mainloop *m) {
    if (m) pthread_mutex_unlock(&m->mutex);
}

void pa_threaded_mainloop_wait(pa_threaded_mainloop *m) {
    if (m) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50000000L; // 50ms timeout
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&m->cond, &m->mutex, &ts);
    }
}

void pa_threaded_mainloop_signal(pa_threaded_mainloop *m, int wait_for_accept) {
    if (m) pthread_cond_broadcast(&m->cond);
}

void *pa_threaded_mainloop_get_api(pa_threaded_mainloop *m) {
    return (void *)m;
}

/* Context API */
pa_context *pa_context_new(void *mainloop_api, const char *name) {
    LOG("pa_context_new: name=%s", name ? name : "null");
    pa_context *c = calloc(1, sizeof(pa_context));
    c->mainloop = (pa_threaded_mainloop *)mainloop_api;
    c->state = PA_CONTEXT_UNCONNECTED;
    return c;
}

pa_context *pa_context_new_with_proplist(void *mainloop_api, const char *name, void *proplist) {
    return pa_context_new(mainloop_api, name);
}

void pa_context_unref(pa_context *c) {
    LOG("pa_context_unref");
    if (c) free(c);
}

void pa_context_set_state_callback(pa_context *c, void (*cb)(pa_context *c, void *userdata), void *userdata) {
    LOG("pa_context_set_state_callback");
    if (c) {
        c->state_callback = cb;
        c->state_userdata = userdata;
    }
}

int pa_context_connect(pa_context *c, const char *server, int flags, void *spawn_api) {
    LOG("pa_context_connect: server=%s", server ? server : "null");
    if (!c) return -1;
    c->state = PA_CONTEXT_READY;
    if (c->state_callback) {
        c->state_callback(c, c->state_userdata);
    }
    if (c->mainloop) {
        pthread_cond_broadcast(&c->mainloop->cond);
    }
    return 0;
}

void pa_context_disconnect(pa_context *c) {
    LOG("pa_context_disconnect");
    if (c) c->state = PA_CONTEXT_TERMINATED;
}

pa_context_state_t pa_context_get_state(const pa_context *c) {
    return c ? c->state : PA_CONTEXT_FAILED;
}

int pa_context_errno(const pa_context *c) { return 0; }
const char *pa_strerror(int error) { return "Success"; }

pa_operation *pa_context_subscribe(pa_context *c, int mask, pa_context_success_cb_t cb, void *userdata) {
    LOG("pa_context_subscribe");
    if (cb && c) cb(c, 1, userdata);
    pa_operation *op = calloc(1, sizeof(pa_operation));
    op->state = PA_OPERATION_DONE;
    return op;
}

void pa_context_set_subscribe_callback(pa_context *c, void *cb, void *userdata) {
    LOG("pa_context_set_subscribe_callback");
    if (c) {
        c->subscribe_callback = cb;
        c->subscribe_userdata = userdata;
    }
}

pa_operation *pa_context_get_sink_input_info(pa_context *c, uint32_t idx, pa_sink_input_info_cb_t cb, void *userdata) {
    LOG("pa_context_get_sink_input_info: idx=%u", idx);
    if (cb && c) {
        pa_sink_input_info info;
        memset(&info, 0, sizeof(info));
        info.index = idx;
        info.name = "Spotify";
        info.corked = 0;
        info.has_volume = 1;
        info.volume_writable = 1;
        info.volume.channels = 2;
        info.volume.values[0] = 65536;
        info.volume.values[1] = 65536;
        cb(c, &info, 0, userdata);
        cb(c, NULL, 1, userdata);
    }
    pa_operation *op = calloc(1, sizeof(pa_operation));
    op->state = PA_OPERATION_DONE;
    return op;
}

pa_operation *pa_context_set_sink_input_volume(pa_context *c, uint32_t idx, const pa_cvolume *cv, pa_context_success_cb_t cb, void *userdata) {
    if (cv && cv->channels > 0) {
        uint64_t sum = 0;
        for (unsigned i = 0; i < cv->channels; i++) sum += cv->values[i];
        uint32_t avg = (uint32_t)(sum / cv->channels);
        int vol_pct = (int)((avg * 100ULL) / 65536ULL);
        LOG("pa_context_set_sink_input_volume: idx=%u, avg=%u (%d%%)", idx, avg, vol_pct);
    }
    if (cb && c) cb(c, 1, userdata);
    pa_operation *op = calloc(1, sizeof(pa_operation));
    op->state = PA_OPERATION_DONE;
    return op;
}

/* Operation API */
pa_operation_state_t pa_operation_get_state(const pa_operation *o) {
    return o ? o->state : PA_OPERATION_DONE;
}

void pa_operation_unref(pa_operation *o) {
    if (o) free(o);
}

/* Proplist API */
void *pa_proplist_new(void) { return (void *)1; }
void pa_proplist_free(void *p) {}
int pa_proplist_sets(void *p, const char *key, const char *val) { return 0; }

/* CVolume API */
pa_cvolume *pa_cvolume_set(pa_cvolume *a, unsigned channels, uint32_t v) {
    if (!a) return NULL;
    a->channels = channels;
    for (unsigned i = 0; i < channels && i < 32; i++) a->values[i] = v;
    return a;
}

uint32_t pa_cvolume_avg(const pa_cvolume *a) {
    if (!a || a->channels == 0) return 65536;
    uint64_t sum = 0;
    for (unsigned i = 0; i < a->channels; i++) sum += a->values[i];
    return (uint32_t)(sum / a->channels);
}

/* Stream API */
pa_stream *pa_stream_new(pa_context *c, const char *name, const pa_sample_spec *ss, void *map) {
    LOG("pa_stream_new: name=%s, format=%d, rate=%u, channels=%u",
        name ? name : "null", ss ? ss->format : -1, ss ? ss->rate : 0, ss ? ss->channels : 0);
    pa_stream *s = calloc(1, sizeof(pa_stream));
    s->context = c;
    if (ss) s->ss = *ss;
    s->state = PA_STREAM_UNCONNECTED;
    s->corked = 1; // Default to corked until connect/cork
    gettimeofday(&s->uncork_time, NULL);
    return s;
}

pa_stream *pa_stream_new_with_proplist(pa_context *c, const char *name, const pa_sample_spec *ss, void *map, void *p) {
    return pa_stream_new(c, name, ss, map);
}

void pa_stream_set_state_callback(pa_stream *s, void (*cb)(pa_stream *s, void *userdata), void *userdata) {
    LOG("pa_stream_set_state_callback");
    if (s) {
        s->state_callback = cb;
        s->state_userdata = userdata;
    }
}

void pa_stream_set_started_callback(pa_stream *s, void (*cb)(pa_stream *s, void *userdata), void *userdata) {
    LOG("pa_stream_set_started_callback: cb=%p, corked=%d, bytes_written=%llu",
        cb, s ? s->corked : -1, s ? (unsigned long long)s->bytes_written : 0);
    if (s) {
        s->started_callback = cb;
        s->started_userdata = userdata;
        check_and_notify_started(s);
    }
}

void pa_stream_set_underflow_callback(pa_stream *s, void (*cb)(pa_stream *s, void *userdata), void *userdata) {
    LOG("pa_stream_set_underflow_callback");
    if (s) {
        s->underflow_callback = cb;
        s->underflow_userdata = userdata;
    }
}

static void open_aplay_pipe(pa_stream *s) {
    if (s->pcm_pipe) return;

    int pfd[2];
    if (pipe(pfd) != 0) {
        LOG("ERROR: pipe() failed");
        return;
    }

    // Expand kernel pipe buffer to 512KB for glitch-free streaming
    fcntl(pfd[0], F_SETPIPE_SZ, 524288);
    fcntl(pfd[1], F_SETPIPE_SZ, 524288);

    uint32_t rate = s->ss.rate ? s->ss.rate : 44100;
    char rate_str[16];
    snprintf(rate_str, sizeof(rate_str), "%u", rate);

    pid_t pid = fork();
    if (pid < 0) {
        LOG("ERROR: fork() failed");
        close(pfd[0]);
        close(pfd[1]);
        return;
    }

    if (pid == 0) {
        close(pfd[1]);
        dup2(pfd[0], STDIN_FILENO);
        close(pfd[0]);

        unsetenv("LD_LIBRARY_PATH");
        unsetenv("LD_PRELOAD");

        // Use plughw:0,0 with 300ms buffer to eliminate ALSA XRUN underruns
        execl("/usr/bin/aplay", "aplay",
              "-D", "plughw:0,0",
              "-f", "S16_LE",
              "-r", rate_str,
              "-c", "2",
              "--buffer-time=300000",
              "--period-time=50000",
              "-q", NULL);
        _exit(127);
    }

    close(pfd[0]);
    s->aplay_pid = pid;
    s->pcm_pipe = fdopen(pfd[1], "w");
    LOG("SUCCESS: opened clean aplay pipeline (pid=%d, rate=%u)", pid, rate);
}

static inline int16_t float_to_s16(float f) {
    // -1.0 dB digital headroom (0.89125) prevents harsh inter-sample clipping/crackling on Vorbis lossy peaks
    float val = f * 0.89125f;
    if (val >= 1.0f) return 32767;
    if (val <= -1.0f) return -32768;
    return (int16_t)lrintf(val * 32767.0f);
}

static void write_pcm_to_pipe(pa_stream *s, const void *data, size_t nbytes) {
    if (!s || !data || nbytes == 0) return;
    if (!s->pcm_pipe) open_aplay_pipe(s);
    if (!s->pcm_pipe) return;

    if (s->ss.format == PA_SAMPLE_FLOAT32LE) {
        const float *in = (const float *)data;
        size_t num_samples = nbytes / sizeof(float);
        int16_t conv_buf[1024];
        size_t idx = 0;
        while (idx < num_samples) {
            size_t chunk = num_samples - idx;
            if (chunk > sizeof(conv_buf)/sizeof(conv_buf[0])) {
                chunk = sizeof(conv_buf)/sizeof(conv_buf[0]);
            }
            for (size_t i = 0; i < chunk; i++) {
                conv_buf[i] = float_to_s16(in[idx + i]);
            }
            fwrite(conv_buf, sizeof(int16_t), chunk, s->pcm_pipe);
            idx += chunk;
        }
    } else {
        fwrite(data, 1, nbytes, s->pcm_pipe);
    }
    fflush(s->pcm_pipe);
}

int pa_stream_connect_playback(pa_stream *s, const char *dev, void *attr, int flags, void *volume, void *sync_stream) {
    LOG("pa_stream_connect_playback: format=%d, rate=%u, channels=%u, flags=0x%x, attr=%p",
        s->ss.format, s->ss.rate, s->ss.channels, flags, attr);
    if (!s) return -1;
    s->state = PA_STREAM_READY;
    
    // Honor PA_STREAM_START_CORKED (0x0001)
    s->corked = (flags & 0x0001) ? 1 : 0;
    s->started_notified = 0;
    s->bytes_written = 0;
    s->read_bytes_at_cork = 0;
    gettimeofday(&s->uncork_time, NULL);

    if (attr) {
        const pa_buffer_attr *a = (const pa_buffer_attr *)attr;
        s->attr.maxlength = (a->maxlength == (uint32_t)-1 || a->maxlength == 0) ? 4194304 : a->maxlength;
        s->attr.tlength = (a->tlength == (uint32_t)-1 || a->tlength == 0) ? 352800 : a->tlength;
        s->attr.prebuf = (a->prebuf == (uint32_t)-1 || a->prebuf == 0) ? 16384 : a->prebuf;
        s->attr.minreq = (a->minreq == (uint32_t)-1 || a->minreq == 0) ? 4096 : a->minreq;
        s->attr.fragsize = (a->fragsize == (uint32_t)-1 || a->fragsize == 0) ? 4096 : a->fragsize;
        LOG("pa_stream_connect_playback sanitized attr: maxlength=%u, tlength=%u, prebuf=%u, minreq=%u",
            s->attr.maxlength, s->attr.tlength, s->attr.prebuf, s->attr.minreq);
    } else {
        s->attr.maxlength = 4194304;
        s->attr.tlength = 352800;
        s->attr.prebuf = 16384;
        s->attr.minreq = 4096;
        s->attr.fragsize = 4096;
    }

    if (s->state_callback) s->state_callback(s, s->state_userdata);
    if (s->context && s->context->mainloop) {
        pthread_cond_broadcast(&s->context->mainloop->cond);
    }
    return 0;
}

pa_stream_state_t pa_stream_get_state(const pa_stream *s) {
    return s ? s->state : PA_STREAM_FAILED;
}

uint32_t pa_stream_get_index(const pa_stream *s) { return 1; }

const pa_buffer_attr *pa_stream_get_buffer_attr(const pa_stream *s) {
    return s ? &s->attr : NULL;
}

const pa_sample_spec *pa_stream_get_sample_spec(const pa_stream *s) {
    return s ? &s->ss : NULL;
}

const pa_timing_info *pa_stream_get_timing_info(pa_stream *s) {
    if (!s) return NULL;
    struct timeval now;
    gettimeofday(&now, NULL);
    s->timing.timestamp = now;
    s->timing.synchronized_clocks = 1;

    uint64_t bps = get_bps(s);
    uint64_t played = s->read_bytes_at_cork;
    if (!s->corked) {
        int64_t elapsed_us = (now.tv_sec - s->uncork_time.tv_sec) * 1000000LL + (now.tv_usec - s->uncork_time.tv_usec);
        if (elapsed_us > 0) {
            played += (uint64_t)(elapsed_us * bps / 1000000ULL);
            if (played > s->bytes_written) played = s->bytes_written;
        }
    }

    s->timing.write_index = (int64_t)s->bytes_written;
    s->timing.read_index = (int64_t)played;
    s->timing.playing = !s->corked && (s->bytes_written > played);

    pa_usec_t max_sink_us = 40000;
    if (s->bytes_written > played) {
        pa_usec_t unplayed_us = (pa_usec_t)((s->bytes_written - played) * 1000000ULL / bps);
        s->timing.sink_usec = (unplayed_us < max_sink_us) ? unplayed_us : max_sink_us;
    } else {
        s->timing.sink_usec = 0;
    }

    return &s->timing;
}

pa_operation *pa_stream_update_timing_info(pa_stream *s, pa_stream_success_cb_t cb, void *userdata) {
    if (s) {
        pa_stream_get_timing_info(s);
    }
    if (cb && s) cb(s, 1, userdata);
    if (s && s->context && s->context->mainloop) {
        pthread_cond_broadcast(&s->context->mainloop->cond);
    }
    pa_operation *op = calloc(1, sizeof(pa_operation));
    op->state = PA_OPERATION_DONE;
    return op;
}

size_t pa_stream_writable_size(const pa_stream *s) {
    if (!s) return 65536;
    uint64_t bps = get_bps(s);
    uint64_t played = s->read_bytes_at_cork;
    if (!s->corked) {
        struct timeval now;
        gettimeofday(&now, NULL);
        int64_t elapsed_us = (now.tv_sec - s->uncork_time.tv_sec) * 1000000LL + (now.tv_usec - s->uncork_time.tv_usec);
        if (elapsed_us > 0) {
            played += (uint64_t)(elapsed_us * bps / 1000000ULL);
            if (played > s->bytes_written) played = s->bytes_written;
        }
    }
    uint64_t unplayed = (s->bytes_written > played) ? (s->bytes_written - played) : 0;
    size_t target_buf = (s->attr.tlength && s->attr.tlength != (uint32_t)-1) ? s->attr.tlength : 352800;
    if (target_buf < 65536) target_buf = 352800;

    if (unplayed >= target_buf) return 0;
    size_t writable = target_buf - (size_t)unplayed;
    return (writable > 65536) ? 65536 : writable;
}

int pa_stream_write(pa_stream *s, const void *data, size_t nbytes, void (*free_cb)(void *p), int64_t offset, int seek) {
    if (s && data && nbytes > 0) {
        static int write_log_cnt = 0;
        if (write_log_cnt++ < 5 || (write_log_cnt % 50 == 0)) {
            LOG("pa_stream_write: nbytes=%zu, corked=%d, total_written=%llu",
                nbytes, s->corked, (unsigned long long)s->bytes_written);
        }

        if (s->corked) {
            // Buffer prebuffering data in memory until uncorked
            if (s->prebuf_len + nbytes > s->prebuf_cap) {
                size_t new_cap = (s->prebuf_cap == 0) ? 65536 : s->prebuf_cap * 2;
                while (new_cap < s->prebuf_len + nbytes) new_cap *= 2;
                s->prebuf = realloc(s->prebuf, new_cap);
                s->prebuf_cap = new_cap;
            }
            memcpy(s->prebuf + s->prebuf_len, data, nbytes);
            s->prebuf_len += nbytes;
        } else {
            write_pcm_to_pipe(s, data, nbytes);
        }

        s->bytes_written += nbytes;
    }

    if (free_cb) free_cb((void*)data);
    return 0;
}

pa_operation *pa_stream_cork(pa_stream *s, int b, pa_stream_success_cb_t cb, void *userdata) {
    LOG("pa_stream_cork: corked=%d, current=%d, bytes_written=%llu", b, s ? s->corked : -1, s ? (unsigned long long)s->bytes_written : 0);
    if (s) {
        struct timeval now;
        gettimeofday(&now, NULL);
        uint64_t bps = get_bps(s);
        if (b) {
            if (!s->corked) {
                int64_t elapsed_us = (now.tv_sec - s->uncork_time.tv_sec) * 1000000LL + (now.tv_usec - s->uncork_time.tv_usec);
                if (elapsed_us > 0) {
                    s->read_bytes_at_cork += (uint64_t)(elapsed_us * bps / 1000000ULL);
                    if (s->read_bytes_at_cork > s->bytes_written) s->read_bytes_at_cork = s->bytes_written;
                }
                s->corked = 1;
            }
        } else {
            // UNCORK: Flush memory prebuffer to aplay and start timing
            if (s->corked) {
                s->corked = 0;
                s->uncork_time = now;
                if (s->prebuf_len > 0) {
                    LOG("pa_stream_cork: flushing %zu bytes prebuffer to aplay pipe", s->prebuf_len);
                    write_pcm_to_pipe(s, s->prebuf, s->prebuf_len);
                    s->prebuf_len = 0;
                }
                check_and_notify_started(s);
            }
        }
    }
    if (cb && s) cb(s, 1, userdata);
    if (s && s->context && s->context->mainloop) {
        pthread_cond_broadcast(&s->context->mainloop->cond);
    }
    pa_operation *op = calloc(1, sizeof(pa_operation));
    op->state = PA_OPERATION_DONE;
    return op;
}

int pa_stream_is_corked(const pa_stream *s) {
    return s ? s->corked : 0;
}

pa_operation *pa_stream_flush(pa_stream *s, pa_stream_success_cb_t cb, void *userdata) {
    LOG("pa_stream_flush");
    if (s) {
        s->read_bytes_at_cork = s->bytes_written;
        s->prebuf_len = 0;
    }
    if (cb && s) cb(s, 1, userdata);
    if (s && s->context && s->context->mainloop) {
        pthread_cond_broadcast(&s->context->mainloop->cond);
    }
    pa_operation *op = calloc(1, sizeof(pa_operation));
    op->state = PA_OPERATION_DONE;
    return op;
}

int pa_stream_disconnect(pa_stream *s) {
    LOG("pa_stream_disconnect");
    if (s) {
        s->state = PA_STREAM_TERMINATED;
        if (s->pcm_pipe) {
            fclose(s->pcm_pipe);
            s->pcm_pipe = NULL;
        }
        if (s->aplay_pid > 0) {
            kill(s->aplay_pid, SIGTERM);
            int status = 0;
            waitpid(s->aplay_pid, &status, 0);
            s->aplay_pid = 0;
        }
        if (s->prebuf) {
            free(s->prebuf);
            s->prebuf = NULL;
            s->prebuf_len = 0;
            s->prebuf_cap = 0;
        }
    }
    return 0;
}

void pa_stream_unref(pa_stream *s) {
    LOG("pa_stream_unref");
    if (!s) return;
    pa_stream_disconnect(s);
    free(s);
}

/* Timeval Utility */
pa_usec_t pa_timeval_diff(const struct timeval *a, const struct timeval *b) {
    int64_t diff = (a->tv_sec - b->tv_sec) * 1000000LL + (a->tv_usec - b->tv_usec);
    return diff > 0 ? (pa_usec_t)diff : 0;
}

pa_usec_t pa_gettimeofday(struct timeval *tv) {
    gettimeofday(tv, NULL);
    return (pa_usec_t)tv->tv_sec * 1000000ULL + tv->tv_usec;
}

size_t pa_usec_to_bytes(pa_usec_t t, const pa_sample_spec *spec) {
    uint32_t rate = spec ? spec->rate : 44100;
    uint32_t ch = spec ? spec->channels : 2;
    uint32_t sample_size = (spec && (spec->format == PA_SAMPLE_FLOAT32LE || spec->format == PA_SAMPLE_S32LE)) ? 4 : 2;
    uint64_t bps = (uint64_t)rate * ch * sample_size;
    return (size_t)(t * bps / 1000000ULL);
}
