#import "OXAudioRingWriter.h"
#import "OXAudioRing.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

@implementation OXAudioRingWriter
{
    int _fd;
    void* _map;
    size_t _mapBytes;
    OXAudioRingHeader* _hdr;
    float* _samples;
    uint32_t _channels;
    uint32_t _capacity;
}

+ (NSString*)defaultPath
{
    return [NSHomeDirectory() stringByAppendingPathComponent:@OX_AUDIO_RING_RELATIVE_PATH];
}

- (instancetype)initWithPath:(NSString*)path
                  sampleRate:(uint32_t)rate
                    channels:(uint32_t)channels
                       scope:(uint32_t)scope
                       error:(NSError**)error
{
    if (!(self = [super init])) return nil;
    _fd = -1;
    _channels = channels ? channels : 2;
    _capacity = OX_AUDIO_RING_DEFAULT_CAPACITY_FRAMES;
    [[NSFileManager defaultManager] createDirectoryAtPath:path.stringByDeletingLastPathComponent
                              withIntermediateDirectories:YES attributes:nil error:nil];
    _mapBytes = OX_AUDIO_RING_HEADER_BYTES + (size_t)_capacity * _channels * sizeof(float);
    _fd = open(path.fileSystemRepresentation, O_RDWR | O_CREAT, 0644);
    if (_fd < 0 || ftruncate(_fd, (off_t)_mapBytes) != 0)
    {
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        if (_fd >= 0) close(_fd);
        return nil;
    }
    _map = mmap(NULL, _mapBytes, PROT_READ | PROT_WRITE, MAP_SHARED, _fd, 0);
    if (_map == MAP_FAILED)
    {
        if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:nil];
        close(_fd);
        return nil;
    }
    _hdr = (OXAudioRingHeader*)_map;
    _samples = (float*)((uint8_t*)_map + OX_AUDIO_RING_HEADER_BYTES);
    // Deactivate while (re)initializing so a reader never mixes formats.
    atomic_store_explicit(&_hdr->active, 0, memory_order_release);
    uint32_t gen = _hdr->magic == OX_AUDIO_RING_MAGIC
                       ? atomic_load_explicit(&_hdr->generation, memory_order_relaxed)
                       : 0;
    _hdr->version = OX_AUDIO_RING_VERSION;
    _hdr->sampleRateHz = rate;
    _hdr->channels = _channels;
    _hdr->capacityFrames = _capacity;
    _hdr->writerPid = getpid();
    _hdr->scope = scope;
    atomic_store_explicit(&_hdr->writeFrame, 0, memory_order_relaxed);
    atomic_store_explicit(&_hdr->generation, gen + 1, memory_order_relaxed);
    memset(_samples, 0, (size_t)_capacity * _channels * sizeof(float));
    _hdr->magic = OX_AUDIO_RING_MAGIC;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    atomic_store_explicit(&_hdr->heartbeatNs,
                          (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec,
                          memory_order_relaxed);
    atomic_store_explicit(&_hdr->active, 1, memory_order_release);
    return self;
}

- (uint64_t)framesWritten
{
    return _hdr ? atomic_load_explicit(&_hdr->writeFrame, memory_order_relaxed) : 0;
}

- (void)writeInterleaved:(const float*)data frames:(uint32_t)frames
{
    if (!_hdr || !data || frames == 0) return;
    if (frames > _capacity)
    {
        data += (size_t)(frames - _capacity) * _channels;
        frames = _capacity;
    }
    uint64_t w = atomic_load_explicit(&_hdr->writeFrame, memory_order_relaxed);
    uint32_t start = (uint32_t)(w & (_capacity - 1));
    uint32_t first = frames < _capacity - start ? frames : _capacity - start;
    memcpy(_samples + (size_t)start * _channels, data, (size_t)first * _channels * sizeof(float));
    if (first < frames)
        memcpy(_samples, data + (size_t)first * _channels,
               (size_t)(frames - first) * _channels * sizeof(float));
    atomic_store_explicit(&_hdr->writeFrame, w + frames, memory_order_release);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    atomic_store_explicit(&_hdr->heartbeatNs,
                          (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec,
                          memory_order_relaxed);
}

- (void)close
{
    if (_hdr) atomic_store_explicit(&_hdr->active, 0, memory_order_release);
    if (_map && _map != MAP_FAILED) munmap(_map, _mapBytes);
    _map = NULL;
    _hdr = NULL;
    if (_fd >= 0) close(_fd);
    _fd = -1;
}

- (void)dealloc { [self close]; }
@end
