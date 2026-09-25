#import "OXHeadsetAudio.h"
#import "OXAudioRingWriter.h"
#import "OXTapCapture.h"

#include <libproc.h>
#include <math.h>
#include <stdatomic.h>
#include <unistd.h>

@implementation OXHeadsetAudio
{
    OXTapCapture* _tap;
    OXAudioRingWriter* _ring;
    _Atomic uint32_t _peakBits;
}

- (BOOL)running { return _tap.running; }
- (double)sampleRate { return _tap.sampleRate; }
- (NSArray<NSNumber*>*)tappedPIDs { return _tap.tappedPIDs ?: @[]; }
- (uint64_t)framesWritten { return _ring.framesWritten; }

- (float)takePeak
{
    uint32_t bits = atomic_exchange(&_peakBits, 0);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

- (BOOL)startWithPIDs:(NSArray<NSNumber*>*)pids
                 mute:(BOOL)mute
                scope:(OXHeadsetAudioScope)scope
             ringPath:(NSString*)ringPath
                error:(NSError**)error
{
    [self stop];
    OXTapCapture* tap = [OXTapCapture new];
    // The ring is created lazily on the first IO cycle's format; but the tap
    // format is known right after start, so create the ring before starting IO
    // by using a two-step start: start tap with a block that reads an ivar.
    __block OXAudioRingWriter* __unsafe_unretained ringRef = nil;
    _Atomic uint32_t* peakBits = &_peakBits;
    BOOL ok = [tap startWithPIDs:pids
                            mute:mute
                     sampleBlock:^(const float* d, uint32_t frames, uint32_t ch, double rate) {
                         (void)rate;
                         OXAudioRingWriter* r = ringRef;
                         if (!r) return;
                         if (ch == 2)
                             [r writeInterleaved:d frames:frames];
                         float peak = 0;
                         uint32_t n = frames * ch;
                         for (uint32_t i = 0; i < n; i += 8)
                         {
                             float a = fabsf(d[i]);
                             if (a > peak) peak = a;
                         }
                         uint32_t bits;
                         memcpy(&bits, &peak, sizeof(bits));
                         uint32_t cur = atomic_load(peakBits);
                         while (bits > cur && !atomic_compare_exchange_weak(peakBits, &cur, bits)) {}
                     }
                           error:error];
    if (!ok) return NO;
    OXAudioRingWriter* ring =
        [[OXAudioRingWriter alloc] initWithPath:ringPath ?: [OXAudioRingWriter defaultPath]
                                     sampleRate:(uint32_t)lround(tap.sampleRate)
                                       channels:2
                                          scope:scope
                                          error:error];
    if (!ring)
    {
        [tap stop];
        return NO;
    }
    _ring = ring;
    _tap = tap;
    ringRef = ring; // published after init; IO block starts forwarding now
    return YES;
}

- (void)stop
{
    [_tap stop]; // blocks until the IOProc is stopped, so the ring is no longer used
    _tap = nil;
    [_ring close];
    _ring = nil;
}

- (void)dealloc { [self stop]; }
@end

NSArray<NSArray*>* OXListProcesses(void)
{
    int n = proc_listallpids(NULL, 0);
    if (n <= 0) return @[];
    pid_t* pids = calloc((size_t)n + 64, sizeof(pid_t));
    n = proc_listallpids(pids, (int)((n + 64) * sizeof(pid_t)));
    NSMutableArray* out = [NSMutableArray arrayWithCapacity:(NSUInteger)n];
    uid_t me = getuid();
    for (int i = 0; i < n; i++)
    {
        struct proc_bsdshortinfo info;
        if (proc_pidinfo(pids[i], PROC_PIDT_SHORTBSDINFO, 0, &info, sizeof(info)) != sizeof(info))
            continue;
        if (info.pbsi_uid != me) continue;
        [out addObject:@[ @(pids[i]), [OXTapCapture nameForPID:pids[i]] ?: @"" ]];
    }
    free(pids);
    return out;
}
