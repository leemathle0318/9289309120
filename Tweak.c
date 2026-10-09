/*
 * VCam LIVE Audio Bridge v0.3 — EXPERIMENTAL build for TikTok audio-input testing.
 *
 * Independent of the original VCam 1.1.0. This tweak replaces the PCM output of
 * AudioUnitRender(bus=1) inside TikTok while a designated UDP 48 kHz mono S16LE
 * source is present. Input from arbitrary devices on the LAN is NOT accepted
 * unless explicitly enabled by placing VCamLiveBridge.enable in TikTok's tmp.
 *
 * This has NOT been tested on an iPhone. It may not be the LIVE upload path.
 * Use on a spare device, not on a production livestream.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <stdatomic.h>
#include <dlfcn.h>
#include <limits.h>
#include <math.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
#define AUDIO_UDP_PORT 39876
#define AUDIO_RATE 48000.0
#define RING_CAPACITY 131072u /* power of 2, 2.73 seconds */
#define RING_MASK (RING_CAPACITY - 1u)
#define MAX_FRAMES 4096u
#define MAX_BUFFERS 4u
#define FMT_LPCM 0x6c70636du
#define FLAG_FLOAT 0x01u
#define FLAG_SIGNED 0x04u
#define FLAG_BIG_ENDIAN 0x02u
#define FLAG_NONINTERLEAVED 0x20u
#define PROP_STREAM_FORMAT 8u
#define SCOPE_OUTPUT 2u

typedef int32_t OSStatus;
typedef uint32_t UInt32;
typedef void *AudioUnit;
typedef struct { double mSampleRate; UInt32 mFormatID; UInt32 mFormatFlags;
    UInt32 mBytesPerPacket; UInt32 mFramesPerPacket; UInt32 mBytesPerFrame;
    UInt32 mChannelsPerFrame; UInt32 mBitsPerChannel; UInt32 mReserved;
} AudioStreamBasicDescription;
typedef struct { UInt32 mNumberChannels; UInt32 mDataByteSize; void *mData; } AudioBuffer;
typedef struct { UInt32 mNumberBuffers; AudioBuffer mBuffers[1]; } AudioBufferList;
typedef OSStatus (*AudioUnitRenderFn)(AudioUnit, UInt32 *, const void *, UInt32, UInt32, AudioBufferList *);
typedef OSStatus (*AudioUnitGetPropertyFn)(AudioUnit, UInt32, UInt32, UInt32, void *, UInt32 *);
typedef void (*MSHookFunctionFn)(void *, void *, void **);

static AudioUnitRenderFn originalRender = NULL;
static AudioUnitGetPropertyFn getProperty = NULL;
static _Atomic uint32_t ringWrite = 0, ringRead = 0;
static int16_t ringData[RING_CAPACITY];
static _Atomic uint64_t lastPacketMs = 0;
static _Atomic uint64_t rxPackets = 0, rxSamples = 0, hookCalls = 0;
static _Atomic uint64_t injected = 0, underflows = 0, unsupported = 0, sourceFallback = 0;
static _Atomic uint64_t socketFailures = 0, shortBuffers = 0;
static _Atomic uint32_t lastBus = 0, lastFormat = 0, lastFlags = 0, lastBits = 0, lastChannels = 0, lastRate = 0;
static _Atomic uint32_t enabled = 0;
static atomic_flag consumerBusy = ATOMIC_FLAG_INIT;
static double fraction = 0.0; /* AudioUnitRender consumer thread, guarded */
static char tmpDir[PATH_MAX];
static char enablePath[PATH_MAX];
static char configPath[PATH_MAX];
static char logPath[PATH_MAX];
static char hookStatus[128];
static struct in_addr allowedHost;
static _Atomic uint32_t hostConfigured = 0;

static uint64_t nowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}
static int isEnabled(void) { return atomic_load_explicit(&enabled, memory_order_acquire) != 0; }

/* The file contains exactly a private IPv4 address for the PC, e.g. 192.168.1.5. */
static void updateSettings(void) {
    FILE *f = fopen(enablePath, "r");
    if (!f) {
        atomic_store(&enabled, 0);
        return;
    }
    fclose(f);
    f = fopen(configPath, "r");
    if (!f) {
        atomic_store(&enabled, 0);
        atomic_store(&hostConfigured, 0);
        return;
    }
    char addr[64] = {0};
    int ok = fgets(addr, sizeof(addr), f) != NULL;
    fclose(f);
    if (!ok) {
        atomic_store(&enabled, 0);
        return;
    }
    addr[strcspn(addr, "\r\n \t")] = '\0';
    struct in_addr ip;
    if (inet_pton(AF_INET, addr, &ip) != 1) {
        atomic_store(&enabled, 0);
        return;
    }
    allowedHost = ip;
    atomic_store(&hostConfigured, 1);
    atomic_store(&enabled, 1);
}

static void pushPCM(const unsigned char *bytes, size_t nbytes) {
    size_t n = nbytes / 2;
    if (n == 0 || n > 8192) return;
    uint32_t write = atomic_load_explicit(&ringWrite, memory_order_relaxed);
    uint32_t read = atomic_load_explicit(&ringRead, memory_order_acquire);
    uint32_t freeCount = RING_CAPACITY - (write - read);
    if (n > freeCount) return; /* Do not overwrite live ring buffers. */
    for (size_t i = 0; i < n; ++i) {
        uint16_t v = (uint16_t)bytes[i*2] | ((uint16_t)bytes[i*2 + 1] << 8);
        ringData[(write + (uint32_t)i) & RING_MASK] = (int16_t)v;
    }
    atomic_store_explicit(&ringWrite, write + (uint32_t)n, memory_order_release);
    atomic_fetch_add(&rxSamples, n);
}

static void *receiverThread(void *ignored) {
    (void)ignored;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) { atomic_fetch_add(&socketFailures, 1); return NULL; }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(AUDIO_UDP_PORT);
    if (bind(fd, (const struct sockaddr *)&local, sizeof(local)) != 0) {
        atomic_fetch_add(&socketFailures, 1);
        close(fd);
        return NULL;
    }
    unsigned char bytes[8192];
    time_t lastConfigCheck = 0;
    for (;;) {
        time_t now = time(NULL);
        if (now != lastConfigCheck) {
            updateSettings();
            lastConfigCheck = now;
        }
        struct sockaddr_in remote = {0};
        socklen_t srclen = sizeof(remote);
        ssize_t got = recvfrom(fd, bytes, sizeof(bytes), 0, (struct sockaddr *)&remote, &srclen);
        if (got <= 0) continue;
        if (!isEnabled() || !atomic_load(&hostConfigured)) continue;
        if (remote.sin_family != AF_INET || remote.sin_addr.s_addr != allowedHost.s_addr) continue;
        /* 16-bit little-endian PCM; UDP packets should preserve even length. */
        if ((got & 1) != 0) continue;
        pushPCM(bytes, (size_t)got);
        atomic_fetch_add(&rxPackets, 1);
        atomic_store_explicit(&lastPacketMs, nowMs(), memory_order_release);
    }
    return NULL;
}

static int getAudioFormat(AudioUnit au, AudioStreamBasicDescription *fmt) {
    if (!getProperty) return 0;
    UInt32 size = sizeof(*fmt);
    memset(fmt, 0, sizeof(*fmt));
    OSStatus err = getProperty(au, PROP_STREAM_FORMAT, SCOPE_OUTPUT, 1, fmt, &size);
    if (err != 0 || size != sizeof(*fmt) || fmt->mFormatID != FMT_LPCM) return 0;
    if (fmt->mSampleRate < 8000.0 || fmt->mSampleRate > 192000.0) return 0;
    if (fmt->mChannelsPerFrame == 0 || fmt->mChannelsPerFrame > 2) return 0;
    if (fmt->mFormatFlags & FLAG_BIG_ENDIAN) return 0;
    int supported = (fmt->mBitsPerChannel == 16 && (fmt->mFormatFlags & FLAG_SIGNED)) ||
                    (fmt->mBitsPerChannel == 32 && (fmt->mFormatFlags & FLAG_FLOAT));
    if (!supported) return 0;
    UInt32 bytesPerSample = fmt->mBitsPerChannel / 8u;
    UInt32 expected = (fmt->mFormatFlags & FLAG_NONINTERLEAVED) ?
                       bytesPerSample : bytesPerSample * fmt->mChannelsPerFrame;
    /* Refuse padded/offset/custom layouts instead of corrupting buffers. */
    if (fmt->mBytesPerFrame != expected || fmt->mFramesPerPacket != 1) return 0;
    return 1;
}

/* Generate a single channel sample stream before duplicating across channels. */
static int replaceBuffers(AudioBufferList *data, const AudioStreamBasicDescription *fmt,
                          UInt32 frames, const int16_t *samples) {
    UInt32 channels = fmt->mChannelsPerFrame;
    UInt32 bytesPerSample = fmt->mBitsPerChannel / 8u;
    int planar = (fmt->mFormatFlags & FLAG_NONINTERLEAVED) != 0;
    if (data == NULL || data->mNumberBuffers == 0 || data->mNumberBuffers > MAX_BUFFERS)
        return 0;
    if (planar) {
        if (data->mNumberBuffers < channels) return 0;
        for (UInt32 c = 0; c < channels; ++c) {
            AudioBuffer *b = &data->mBuffers[c];
            if (!b->mData || b->mDataByteSize < frames * bytesPerSample) return 0;
        }
        for (UInt32 c = 0; c < channels; ++c) {
            AudioBuffer *b = &data->mBuffers[c];
            if (bytesPerSample == 2) {
                int16_t *out = (int16_t *)b->mData;
                for (UInt32 i = 0; i < frames; ++i) out[i] = samples[i];
            } else {
                float *out = (float *)b->mData;
                for (UInt32 i = 0; i < frames; ++i) out[i] = (float)samples[i] / 32768.0f;
            }
        }
    } else {
        AudioBuffer *b = &data->mBuffers[0];
        if (!b->mData || b->mDataByteSize < frames * bytesPerSample * channels) return 0;
        if (bytesPerSample == 2) {
            int16_t *out = (int16_t *)b->mData;
            for (UInt32 i = 0; i < frames; ++i)
                for (UInt32 c = 0; c < channels; ++c) out[i * channels + c] = samples[i];
        } else {
            float *out = (float *)b->mData;
            for (UInt32 i = 0; i < frames; ++i)
                for (UInt32 c = 0; c < channels; ++c) out[i * channels + c] = (float)samples[i] / 32768.0f;
        }
    }
    return 1;
}

static OSStatus hookedRender(AudioUnit au, UInt32 *flags, const void *ts,
                            UInt32 bus, UInt32 frames, AudioBufferList *data) {
    if (!originalRender) return -1;
    OSStatus status = originalRender(au, flags, ts, bus, frames, data);
    atomic_fetch_add(&hookCalls, 1);
    atomic_store(&lastBus, bus);
    if (status != 0 || bus != 1 || frames == 0 || frames > MAX_FRAMES || !data)
        return status;
    if (!isEnabled()) return status;
    uint64_t last = atomic_load_explicit(&lastPacketMs, memory_order_acquire);
    uint64_t now = nowMs();
    if (!last || now < last || now - last > 1500) {
        atomic_fetch_add(&sourceFallback, 1);
        return status; /* No stream: keep original microphone. */
    }
    if (atomic_flag_test_and_set_explicit(&consumerBusy, memory_order_acquire))
        return status;
    AudioStreamBasicDescription fmt;
    if (!getAudioFormat(au, &fmt)) {
        atomic_fetch_add(&unsupported, 1);
        atomic_flag_clear_explicit(&consumerBusy, memory_order_release);
        return status;
    }
    atomic_store(&lastFormat, fmt.mFormatID);
    atomic_store(&lastFlags, fmt.mFormatFlags);
    atomic_store(&lastBits, fmt.mBitsPerChannel);
    atomic_store(&lastChannels, fmt.mChannelsPerFrame);
    atomic_store(&lastRate, (uint32_t)(fmt.mSampleRate + 0.5));
    double step = AUDIO_RATE / fmt.mSampleRate;
    uint32_t read = atomic_load_explicit(&ringRead, memory_order_relaxed);
    uint32_t write = atomic_load_explicit(&ringWrite, memory_order_acquire);
    uint32_t available = write - read;
    /* Keep about 80 ms of audio to reduce cumulative stream delay. */
    if (available > 12000) {
        read = write - 3840;
        available = 3840;
        fraction = 0.0;
    }
    uint32_t needed = (uint32_t)(fraction + (double)frames * step) + 2u;
    int16_t samples[MAX_FRAMES];
    if (available < needed) {
        memset(samples, 0, frames * sizeof(int16_t));
        atomic_fetch_add(&underflows, 1);
        /* When stream is active but runs out, output silence, not live mic. */
    } else {
        for (UInt32 i = 0; i < frames; ++i) {
            uint32_t sourceOffset = (uint32_t)fraction;
            samples[i] = ringData[(read + sourceOffset) & RING_MASK];
            fraction += step;
        }
        uint32_t advanced = (uint32_t)fraction;
        fraction -= (double)advanced;
        read += advanced;
        atomic_store_explicit(&ringRead, read, memory_order_release);
    }
    if (replaceBuffers(data, &fmt, frames, samples))
        atomic_fetch_add(&injected, 1);
    else atomic_fetch_add(&shortBuffers, 1);
    atomic_flag_clear_explicit(&consumerBusy, memory_order_release);
    return status;
}

static void *loggerThread(void *ignored) {
    (void)ignored;
    FILE *f = fopen(logPath, "a");
    if (f) {
        fprintf(f, "# VCam LIVE Audio Bridge v0.3 EXPERIMENTAL / 48 kHz S16LE mono UDP %d\n# %s\n",
                AUDIO_UDP_PORT, hookStatus);
        fclose(f);
    }
    for (;;) {
        sleep(5);
        f = fopen(logPath, "a");
        if (!f) continue;
        fprintf(f, "epoch=%lld enabled=%u sourceIPConfigured=%u udpPackets=%llu udpSamples=%llu "
                "AURender=%llu lastBus=%u injected=%llu underflows=%llu "
                "unsupported=%llu shortBuffers=%llu fallbackToMic=%llu "
                "socketFailures=%llu format=0x%08x flags=0x%x bits=%u channels=%u rate=%u\n",
                (long long)time(NULL), (unsigned)atomic_load(&enabled),
                (unsigned)atomic_load(&hostConfigured),
                (unsigned long long)atomic_load(&rxPackets),
                (unsigned long long)atomic_load(&rxSamples),
                (unsigned long long)atomic_load(&hookCalls),
                (unsigned)atomic_load(&lastBus),
                (unsigned long long)atomic_load(&injected),
                (unsigned long long)atomic_load(&underflows),
                (unsigned long long)atomic_load(&unsupported),
                (unsigned long long)atomic_load(&shortBuffers),
                (unsigned long long)atomic_load(&sourceFallback),
                (unsigned long long)atomic_load(&socketFailures),
                (unsigned)atomic_load(&lastFormat),
                (unsigned)atomic_load(&lastFlags),
                (unsigned)atomic_load(&lastBits),
                (unsigned)atomic_load(&lastChannels),
                (unsigned)atomic_load(&lastRate));
        fclose(f);
    }
    return NULL;
}

__attribute__((constructor)) static void initializeBridge(void) {
    const char *temp = getenv("TMPDIR");
    if (!temp || !temp[0]) temp = "/tmp";
    snprintf(tmpDir, sizeof(tmpDir), "%s", temp);
    size_t len = strlen(tmpDir);
    if (!len || len > PATH_MAX - 60) return;
    const char *slash = tmpDir[len - 1] == '/' ? "" : "/";
    snprintf(enablePath, sizeof(enablePath), "%s%sVCamLiveBridge.enable", tmpDir, slash);
    snprintf(configPath, sizeof(configPath), "%s%sVCamLiveBridge.pc-ip", tmpDir, slash);
    snprintf(logPath, sizeof(logPath), "%s%sVCamLiveBridge.log", tmpDir, slash);
    void *toolbox = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_NOW | RTLD_GLOBAL);
    void *(*lookup)(void *, const char *) = dlsym;
    void *target = lookup(toolbox ? toolbox : RTLD_DEFAULT, "AudioUnitRender");
    getProperty = (AudioUnitGetPropertyFn)lookup(toolbox ? toolbox : RTLD_DEFAULT, "AudioUnitGetProperty");
    MSHookFunctionFn hooker = (MSHookFunctionFn)lookup(RTLD_DEFAULT, "MSHookFunction");
    if (hooker && target && getProperty) hooker(target, (void *)&hookedRender, (void **)&originalRender);
    snprintf(hookStatus, sizeof(hookStatus), "MSHookFunction=%s AudioUnitRender=%s GetProperty=%s",
             hooker ? "found" : "MISSING", originalRender ? "hooked" : "NOT_HOOKED",
             getProperty ? "found" : "MISSING");
    pthread_t t;
    if (pthread_create(&t, NULL, receiverThread, NULL) == 0) pthread_detach(t);
    if (pthread_create(&t, NULL, loggerThread, NULL) == 0) pthread_detach(t);
}
