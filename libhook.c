#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <link.h>
#include <stdint.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <immintrin.h>

#ifdef WEMEET_DEBUG
#define DEBUG_LOG(fmt, ...) fprintf(stderr, "[libhook] " fmt "\n", ##__VA_ARGS__)
#else
#define DEBUG_LOG(fmt, ...) do {} while(0)
#endif

// --- CPU Feature Detection ---
static int has_avx512 = 0;
static int has_avx2 = 0;

__attribute__((constructor)) static void init_cpu_features() {
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx512bw")) has_avx512 = 1;
    if (__builtin_cpu_supports("avx2")) has_avx2 = 1;
    DEBUG_LOG("CPU features initialized. AVX-512: %d, AVX2: %d", has_avx512, has_avx2);
}

// --- High Performance Fake Buffer Pool ---
struct fake_frame {
    struct pw_buffer pw;
    struct spa_buffer spa;
    struct spa_data datas[1];
    struct spa_chunk chunk;
    void *payload;
    size_t capacity;
    int in_use;
};

#define MAX_FAKE_FRAMES 8
static struct fake_frame fake_frames[MAX_FAKE_FRAMES];
static pthread_mutex_t ff_mutex = PTHREAD_MUTEX_INITIALIZER;

static struct fake_frame *alloc_fake_frame(size_t size) {
    pthread_mutex_lock(&ff_mutex);
    struct fake_frame *ret = NULL;
    for (int i = 0; i < MAX_FAKE_FRAMES; i++) {
        if (!fake_frames[i].in_use) {
            if (fake_frames[i].capacity < size) {
                // Ensure 64-byte alignment for AVX-512
                if (fake_frames[i].payload) free(fake_frames[i].payload);
                if (posix_memalign(&fake_frames[i].payload, 64, size) != 0) {
                    fake_frames[i].payload = NULL;
                    fake_frames[i].capacity = 0;
                    pthread_mutex_unlock(&ff_mutex);
                    return NULL;
                }
                fake_frames[i].capacity = size;
                DEBUG_LOG("Allocated new payload buffer of size %zu for slot %d", size, i);
            }
            fake_frames[i].in_use = 1;
            ret = &fake_frames[i];
            break;
        }
    }
    pthread_mutex_unlock(&ff_mutex);
    if (!ret) DEBUG_LOG("WARNING: Fake frame pool exhausted!");
    return ret;
}

static int free_fake_frame(struct pw_buffer *pw) {
    int found = 0;
    pthread_mutex_lock(&ff_mutex);
    for (int i = 0; i < MAX_FAKE_FRAMES; i++) {
        if (&fake_frames[i].pw == pw) {
            fake_frames[i].in_use = 0;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&ff_mutex);
    return found;
}

// --- Real PipeWire APIs ---
static void *(*real_dlsym)(void *handle, const char *symbol) = NULL;
static struct pw_buffer *(*real_pw_stream_dequeue_buffer)(struct pw_stream *stream) = NULL;
static int (*real_pw_stream_queue_buffer)(struct pw_stream *stream, struct pw_buffer *buffer) = NULL;

// --- Fast Copy Implementations ---
static const uint8_t swap_mask_arr[64] __attribute__((aligned(64))) = {
    2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15,
    2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15,
    2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15,
    2, 1, 0, 3, 6, 5, 4, 7, 10, 9, 8, 11, 14, 13, 12, 15
};

__attribute__((target("avx512bw")))
static inline void copy_swap_avx512(uint8_t *dst, const uint8_t *src, size_t size) {
    size_t i = 0;
    const __m512i mask = _mm512_load_si512((const __m512i*)swap_mask_arr);
    for (; i + 64 <= size; i += 64) {
        __m512i v = _mm512_loadu_si512((const __m512i*)(src + i));
        v = _mm512_shuffle_epi8(v, mask);
        _mm512_storeu_si512((__m512i*)(dst + i), v);
    }
    for (; i < size; i += 4) {
        dst[i]   = src[i+2];
        dst[i+1] = src[i+1];
        dst[i+2] = src[i];
        dst[i+3] = src[i+3];
    }
}

__attribute__((target("avx2")))
static inline void copy_swap_avx2(uint8_t *dst, const uint8_t *src, size_t size) {
    size_t i = 0;
    const __m256i mask = _mm256_load_si256((const __m256i*)swap_mask_arr);
    for (; i + 32 <= size; i += 32) {
        __m256i v = _mm256_loadu_si256((const __m256i*)(src + i));
        v = _mm256_shuffle_epi8(v, mask);
        _mm256_storeu_si256((__m256i*)(dst + i), v);
    }
    for (; i < size; i += 4) {
        dst[i]   = src[i+2];
        dst[i+1] = src[i+1];
        dst[i+2] = src[i];
        dst[i+3] = src[i+3];
    }
}

// Highly optimized copy, supports optional color swap
static inline void fast_copy_image(void *dst, int dst_stride, const void *src, int src_stride, int width, int height, int do_swap) {
    if (src_stride == width && dst_stride == width) {
        size_t total_size = (size_t)width * height;
        if (do_swap) {
            if (has_avx512) copy_swap_avx512(dst, src, total_size);
            else if (has_avx2) copy_swap_avx2(dst, src, total_size);
            else {
                for (size_t i = 0; i < total_size; i += 4) {
                    ((uint8_t*)dst)[i]   = ((uint8_t*)src)[i+2];
                    ((uint8_t*)dst)[i+1] = ((uint8_t*)src)[i+1];
                    ((uint8_t*)dst)[i+2] = ((uint8_t*)src)[i];
                    ((uint8_t*)dst)[i+3] = ((uint8_t*)src)[i+3];
                }
            }
        } else {
            memcpy(dst, src, total_size); // glibc memcpy is highly optimized (often AVX-512 internally)
        }
    } else {
        for (int y = 0; y < height; y++) {
            uint8_t *d = (uint8_t *)dst + y * dst_stride;
            const uint8_t *s = (const uint8_t *)src + y * src_stride;
            if (do_swap) {
                if (has_avx512) copy_swap_avx512(d, s, width);
                else if (has_avx2) copy_swap_avx2(d, s, width);
                else {
                    for (int i = 0; i < width; i += 4) {
                        d[i]   = s[i+2];
                        d[i+1] = s[i+1];
                        d[i+2] = s[i];
                        d[i+3] = s[i+3];
                    }
                }
            } else {
                memcpy(d, s, width);
            }
        }
    }
}


// --- Dynamic Trampoline Hooking (libxcast optimization) ---
static void *copy_image_gateway = NULL;
typedef void (*copy_image_func_t)(const void *src, int src_stride, void *dst, int dst_stride, int width, int height);

static const uint8_t COPY_IMAGE_SIG[] = {
    0x41, 0x57, 0x41, 0x56, 0x45, 0x89, 0xc6, 0x41, 0x55, 
    0x41, 0x54, 0x55, 0x53, 0x48, 0x83, 0xec, 0x18, 0x45, 0x85, 0xc0
};
#define SIG_LEN sizeof(COPY_IMAGE_SIG)
#define JMP_LEN 17 // We overwrite exactly 17 bytes for a 14-byte absolute JMP

struct libxcast_info {
    uintptr_t base;
    size_t size;
    void *copy_image_addr;
};

static int phdr_callback(struct dl_phdr_info *info, size_t size, void *data) {
    if (strstr(info->dlpi_name, "libxcast.so")) {
        struct libxcast_info *xinfo = (struct libxcast_info *)data;
        for (int i = 0; i < info->dlpi_phnum; i++) {
            if (info->dlpi_phdr[i].p_type == PT_LOAD && (info->dlpi_phdr[i].p_flags & PF_X)) {
                xinfo->base = info->dlpi_addr + info->dlpi_phdr[i].p_vaddr;
                xinfo->size = info->dlpi_phdr[i].p_memsz;
                uint8_t *p = (uint8_t *)xinfo->base;
                for (size_t offset = 0; offset < xinfo->size - SIG_LEN; offset++) {
                    if (memcmp(p + offset, COPY_IMAGE_SIG, SIG_LEN) == 0) {
                        xinfo->copy_image_addr = p + offset;
                        return 1;
                    }
                }
            }
        }
    }
    return 0;
}

// Our Intercepted libxcast.so copy_image
// Wemeet (libyuv) uses: (src, src_stride, dst, dst_stride, width, height)
static void hook_copy_image(const void *src, int src_stride, void *dst, int dst_stride, int width, int height) {
    // Pure memory copy acceleration. We don't touch PipeWire here!
    fast_copy_image(dst, dst_stride, src, src_stride, width, height, 0);
}

static void install_trampoline_once() {
    static int installed = 0;
    static pthread_mutex_t install_mutex = PTHREAD_MUTEX_INITIALIZER;
    
    pthread_mutex_lock(&install_mutex);
    if (installed) {
        pthread_mutex_unlock(&install_mutex);
        return;
    }
    
    struct libxcast_info info = {0};
    dl_iterate_phdr(phdr_callback, &info);
    
    if (info.copy_image_addr) {
        uint8_t *orig = (uint8_t *)info.copy_image_addr;
        
        copy_image_gateway = mmap(NULL, 128, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        memcpy(copy_image_gateway, orig, JMP_LEN);
        
        uint8_t *gw = (uint8_t *)copy_image_gateway;
        gw[JMP_LEN] = 0xFF; gw[JMP_LEN+1] = 0x25;
        memset(gw+JMP_LEN+2, 0, 4);
        *(void**)(gw+JMP_LEN+6) = (void*)(orig + JMP_LEN);
        
        void *page_start = (void *)((uintptr_t)orig & ~(4095UL));
        mprotect(page_start, 8192, PROT_READ | PROT_WRITE | PROT_EXEC);
        
        orig[0] = 0xFF; orig[1] = 0x25;
        memset(orig+2, 0, 4);
        *(void**)(orig+6) = (void*)hook_copy_image;
        
        installed = 1;
        DEBUG_LOG("Successfully planted trampoline at %p (libxcast base %p)", orig, (void*)info.base);
    } else {
        DEBUG_LOG("Failed to find libxcast.so copy_image signature!");
    }
    pthread_mutex_unlock(&install_mutex);
}

// --- Hooked PipeWire APIs ---

struct pw_buffer *hook_pw_stream_dequeue_buffer(struct pw_stream *stream) {
    install_trampoline_once();
    
    if (!real_pw_stream_dequeue_buffer) return NULL;
    
    struct pw_buffer *pw = real_pw_stream_dequeue_buffer(stream);
    if (!pw || !pw->buffer || pw->buffer->n_datas == 0) return pw;
    
    struct spa_data *real_data = &pw->buffer->datas[0];
    size_t size = real_data->chunk->size;
    if (size == 0) return pw;
    
    struct fake_frame *ff = alloc_fake_frame(size);
    if (!ff) {
        DEBUG_LOG("ERROR: alloc_fake_frame failed, dropping frame!");
        real_pw_stream_queue_buffer(stream, pw);
        return NULL;
    }
    
    int do_swap = 0;
#ifdef WEMEET_HOOK_MODE_SWAP
    do_swap = 1;
#endif

    // Fast copy and swap in the PipeWire thread
    fast_copy_image(ff->payload, size, real_data->data, size, size, 1, do_swap);
    
    // IMMEDIATELY queue back the real buffer to PipeWire (THREAD SAFE)
    real_pw_stream_queue_buffer(stream, pw);
    
    // Construct fake pw_buffer for Wemeet
    memcpy(&ff->pw, pw, sizeof(struct pw_buffer));
    ff->pw.buffer = &ff->spa;
    
    memcpy(&ff->spa, pw->buffer, sizeof(struct spa_buffer));
    ff->spa.datas = ff->datas;
    
    memcpy(&ff->datas[0], real_data, sizeof(struct spa_data));
    ff->datas[0].data = ff->payload;
    ff->datas[0].maxsize = ff->capacity;
    
    memcpy(&ff->chunk, real_data->chunk, sizeof(struct spa_chunk));
    ff->datas[0].chunk = &ff->chunk;
    
    return &ff->pw;
}

int hook_pw_stream_queue_buffer(struct pw_stream *stream, struct pw_buffer *buffer) {
    if (!real_pw_stream_queue_buffer) return -1;
    
    // If it's our fake buffer, just free it and do NOT call PipeWire API
    if (free_fake_frame(buffer)) {
        return 0;
    }
    
    return real_pw_stream_queue_buffer(stream, buffer);
}

// --- Hook dlsym ---
void *dlsym(void *handle, const char *symbol) {
    if (!real_dlsym) {
        real_dlsym = (void *(*)(void *, const char *)) dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
        if (!real_dlsym) {
            real_dlsym = (void *(*)(void *, const char *)) dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.0");
        }
    }
    
    if (strcmp(symbol, "pw_stream_dequeue_buffer") == 0) {
        if (!real_pw_stream_dequeue_buffer) real_pw_stream_dequeue_buffer = real_dlsym(handle, symbol);
        return (void *)hook_pw_stream_dequeue_buffer;
    }
    
    if (strcmp(symbol, "pw_stream_queue_buffer") == 0) {
        if (!real_pw_stream_queue_buffer) real_pw_stream_queue_buffer = real_dlsym(handle, symbol);
        return (void *)hook_pw_stream_queue_buffer;
    }
    
    return real_dlsym(handle, symbol);
}

__attribute__((constructor)) static void init_dlsym() {
    if (!real_dlsym) {
        dlsym(RTLD_NEXT, "init"); 
    }
}
