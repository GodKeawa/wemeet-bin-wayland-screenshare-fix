#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <immintrin.h>

// Function pointers for real libpipewire
static void *(*real_dlsym)(void *handle, const char *symbol) = NULL;
static struct pw_buffer *(*real_pw_stream_dequeue_buffer)(struct pw_stream *stream) = NULL;
static int (*real_pw_stream_queue_buffer)(struct pw_stream *stream, struct pw_buffer *buffer) = NULL;

#define MAX_FAKE_BUFFERS 16

struct fake_pw_buffer {
    struct pw_buffer pw;
    struct spa_buffer spa;
    struct spa_meta metas[4];
    struct spa_data datas[4];
    struct spa_chunk chunks[4];
    void *payload;
    size_t payload_capacity;
    int in_use;
};

static struct fake_pw_buffer fake_buffers[MAX_FAKE_BUFFERS];
static pthread_mutex_t fb_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct fake_pw_buffer *alloc_fake_buffer() {
    pthread_mutex_lock(&fb_mutex);
    for (int i = 0; i < MAX_FAKE_BUFFERS; i++) {
        if (!fake_buffers[i].in_use) {
            fake_buffers[i].in_use = 1;
            pthread_mutex_unlock(&fb_mutex);
            return &fake_buffers[i];
        }
    }
    pthread_mutex_unlock(&fb_mutex);
    return NULL;
}

static void free_fake_buffer(struct fake_pw_buffer *fb) {
    pthread_mutex_lock(&fb_mutex);
    fb->in_use = 0;
    pthread_mutex_unlock(&fb_mutex);
}

// BGRx to RGBx swap using AVX2
static void swap_colors_avx2(uint8_t *dst, const uint8_t *src, size_t size) {
    size_t i = 0;
    // B G R x -> R G B x
    // byte 0 <-> byte 2
    const __m256i mask = _mm256_setr_epi8(
        2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15,
        18, 17, 16, 19, 22, 21, 20, 23, 26, 25, 24, 27, 30, 29, 28, 31
    );
    for (; i + 32 <= size; i += 32) {
        __m256i v = _mm256_loadu_si256((const __m256i*)(src + i));
        v = _mm256_shuffle_epi8(v, mask);
        _mm256_storeu_si256((__m256i*)(dst + i), v);
    }
    for (; i < size; i += 4) {
        uint8_t b = src[i];
        uint8_t g = src[i+1];
        uint8_t r = src[i+2];
        uint8_t x = src[i+3];
        dst[i]   = r;
        dst[i+1] = g;
        dst[i+2] = b;
        dst[i+3] = x;
    }
}

// Hooked functions
struct pw_buffer *hook_pw_stream_dequeue_buffer(struct pw_stream *stream) {
    if (!real_pw_stream_dequeue_buffer) return NULL;
    
    struct pw_buffer *real_pw = real_pw_stream_dequeue_buffer(stream);
    if (!real_pw || !real_pw->buffer || real_pw->buffer->n_datas == 0) {
        return real_pw;
    }
    
    struct spa_data *real_data = &real_pw->buffer->datas[0];
    if (!real_data->data || !real_data->chunk) {
        return real_pw;
    }
    
    size_t size = real_data->chunk->size;
    if (size == 0) {
        return real_pw;
    }
    
    struct fake_pw_buffer *fb = alloc_fake_buffer();
    if (!fb) {
        fprintf(stderr, "[libhook] Out of fake buffers!\n");
        return real_pw; // fallback
    }
    
    if (fb->payload_capacity < size) {
        free(fb->payload);
        fb->payload = malloc(size);
        fb->payload_capacity = size;
    }
    
#ifdef WEMEET_HOOK_MODE_SWAP
    swap_colors_avx2((uint8_t*)fb->payload, (const uint8_t*)real_data->data, size);
#else
    memcpy(fb->payload, real_data->data, size);
#endif
    
    // Construct fake pw_buffer
    memcpy(&fb->pw, real_pw, sizeof(struct pw_buffer));
    memcpy(&fb->spa, real_pw->buffer, sizeof(struct spa_buffer));
    fb->pw.buffer = &fb->spa;
    
    uint32_t n_metas = real_pw->buffer->n_metas > 4 ? 4 : real_pw->buffer->n_metas;
    uint32_t n_datas = real_pw->buffer->n_datas > 4 ? 4 : real_pw->buffer->n_datas;
    
    if (n_metas > 0) {
        memcpy(fb->metas, real_pw->buffer->metas, n_metas * sizeof(struct spa_meta));
        fb->spa.metas = fb->metas;
    }
    if (n_datas > 0) {
        memcpy(fb->datas, real_pw->buffer->datas, n_datas * sizeof(struct spa_data));
        memcpy(fb->chunks, real_pw->buffer->datas[0].chunk, n_datas * sizeof(struct spa_chunk));
        fb->spa.datas = fb->datas;
        for (uint32_t i = 0; i < n_datas; i++) {
            fb->datas[i].chunk = &fb->chunks[i];
        }
    }
    
    // Point the first data chunk to our private payload
    fb->datas[0].data = fb->payload;
    
    // IMMEDIATELY return the real buffer to PipeWire to avoid pool exhaustion!
    real_pw_stream_queue_buffer(stream, real_pw);
    
    return &fb->pw;
}

int hook_pw_stream_queue_buffer(struct pw_stream *stream, struct pw_buffer *buffer) {
    if (!real_pw_stream_queue_buffer) return -1;
    
    // Check if this is one of our fake buffers
    for (int i = 0; i < MAX_FAKE_BUFFERS; i++) {
        if (&fake_buffers[i].pw == buffer) {
            free_fake_buffer(&fake_buffers[i]);
            return 0; // successfully "queued" (actually just freed our struct)
        }
    }
    
    // Otherwise, it's a real buffer (or we are in fallback mode)
    return real_pw_stream_queue_buffer(stream, buffer);
}

// Hook dlsym to intercept PipeWire dynamical loading
void *dlsym(void *handle, const char *symbol) {
    if (!real_dlsym) {
        // Find the real dlsym from libc/libdl
        real_dlsym = (void *(*)(void *, const char *)) dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
        if (!real_dlsym) {
            // Fallback for older glibc or different arch
            real_dlsym = (void *(*)(void *, const char *)) dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.0");
        }
    }
    
    if (strcmp(symbol, "pw_stream_dequeue_buffer") == 0) {
        if (!real_pw_stream_dequeue_buffer) {
            real_pw_stream_dequeue_buffer = real_dlsym(handle, symbol);
        }
        return (void *)hook_pw_stream_dequeue_buffer;
    }
    
    if (strcmp(symbol, "pw_stream_queue_buffer") == 0) {
        if (!real_pw_stream_queue_buffer) {
            real_pw_stream_queue_buffer = real_dlsym(handle, symbol);
        }
        return (void *)hook_pw_stream_queue_buffer;
    }
    
    return real_dlsym(handle, symbol);
}

// Ensure constructor initializes real_dlsym early
__attribute__((constructor)) static void init() {
    dlsym(RTLD_NEXT, "init"); // dummy call to trigger initialization
}
