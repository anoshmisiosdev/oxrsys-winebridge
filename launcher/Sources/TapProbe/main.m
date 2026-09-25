// TapProbe — phase-1 feasibility probe for Core Audio process taps on Wine games.
//
// Launch through LaunchServices so TCC attributes the capture to this app:
//   open -n -W --stdout /tmp/tapprobe/out.log --stderr /tmp/tapprobe/out.log \
//        build/TapProbe.app --args <mode> [options]
// Modes:
//   list                  list Core Audio process objects (PID, name, running I/O)
//   wine                  tap every Wine process that is currently running output
//   pid <pid>[,<pid>...]  tap the given PIDs
//   global                tap all system audio
// Options: --seconds N (default 10)  --mute  --wav <path>

#import <Foundation/Foundation.h>
#import "OXTapCapture.h"
#include <math.h>
#include <stdatomic.h>

static BOOL IsWineName(NSString* name)
{
    return [name containsString:@":\\"] || [name.lowercaseString hasSuffix:@".exe"] ||
           [name containsString:@"wineserver"] || [name containsString:@"wine64"] ||
           [name containsString:@"CrossOver"];
}

static void ListProcesses(void)
{
    for (OXAudioProcessInfo* p in [OXTapCapture audioProcesses])
    {
        printf("pid=%-6d obj=%-4u out=%d in=%d wine=%d bundle=%s name=%s\n", p.pid, p.objectID,
               p.runningOutput, p.runningInput, IsWineName(p.name),
               p.bundleID.UTF8String ?: "-", p.name.UTF8String);
    }
    fflush(stdout);
}

static void WriteWav(NSString* path, const float* data, size_t frames, uint32_t ch, uint32_t rate)
{
    FILE* f = fopen(path.fileSystemRepresentation, "wb");
    if (!f) return;
    uint32_t dataBytes = (uint32_t)(frames * ch * sizeof(float));
    uint32_t riff = 36 + dataBytes, fmtLen = 16, byteRate = rate * ch * 4;
    uint16_t fmtTag = 3, chans = (uint16_t)ch, align = (uint16_t)(ch * 4), bits = 32;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmtLen, 4, 1, f); fwrite(&fmtTag, 2, 1, f); fwrite(&chans, 2, 1, f);
    fwrite(&rate, 4, 1, f); fwrite(&byteRate, 4, 1, f); fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
    fwrite(data, sizeof(float), frames * ch, f);
    fclose(f);
}

int main(int argc, const char* argv[])
{
    @autoreleasepool
    {
        NSArray<NSString*>* args = [NSProcessInfo processInfo].arguments;
        NSString* mode = args.count > 1 ? args[1] : @"list";
        double seconds = 10;
        BOOL mute = NO;
        NSString* wav = @"/tmp/tapprobe/capture.wav";
        NSMutableArray<NSNumber*>* pids = [NSMutableArray array];
        for (NSUInteger i = 2; i < args.count; i++)
        {
            if ([args[i] isEqual:@"--seconds"] && i + 1 < args.count) seconds = args[++i].doubleValue;
            else if ([args[i] isEqual:@"--mute"]) mute = YES;
            else if ([args[i] isEqual:@"--wav"] && i + 1 < args.count) wav = args[++i];
            else
                for (NSString* s in [args[i] componentsSeparatedByString:@","])
                    if (s.intValue > 0) [pids addObject:@(s.intValue)];
        }
        [[NSFileManager defaultManager] createDirectoryAtPath:wav.stringByDeletingLastPathComponent
                                  withIntermediateDirectories:YES attributes:nil error:nil];

        printf("TapProbe pid=%d mode=%s mute=%d seconds=%.0f\n", getpid(), mode.UTF8String, mute,
               seconds);
        ListProcesses();
        if ([mode isEqual:@"list"]) return 0;

        if ([mode isEqual:@"wine"])
        {
            for (OXAudioProcessInfo* p in [OXTapCapture audioProcesses])
                if (IsWineName(p.name) && p.runningOutput) [pids addObject:@(p.pid)];
            if (pids.count == 0)
            {
                printf("RESULT: no Wine process is running audio output\n");
                return 2;
            }
        }
        else if ([mode isEqual:@"global"])
        {
            [pids removeAllObjects];
        }

        const size_t maxFrames = (size_t)(seconds * 48000 * 2) + 48000;
        float* store = calloc(maxFrames * 2, sizeof(float));
        __block _Atomic size_t storedFrames = 0;
        __block _Atomic uint64_t totalFrames = 0;
        __block double sumSq = 0; // written only on IO thread, read approx. on main thread
        __block float peak = 0;
        __block _Atomic uint32_t callbacks = 0;

        OXTapCapture* tap = [OXTapCapture new];
        NSError* err = nil;
        BOOL ok = [tap startWithPIDs:pids
                                mute:mute
                         sampleBlock:^(const float* d, uint32_t frames, uint32_t ch, double rate) {
                             (void)rate;
                             atomic_fetch_add(&callbacks, 1);
                             size_t at = atomic_load(&storedFrames);
                             for (uint32_t f = 0; f < frames; f++)
                             {
                                 float l = d[f * ch], r = ch > 1 ? d[f * ch + 1] : l;
                                 sumSq += (double)l * l + (double)r * r;
                                 float a = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
                                 if (a > peak) peak = a;
                                 if (at + f < maxFrames)
                                 {
                                     store[(at + f) * 2] = l;
                                     store[(at + f) * 2 + 1] = r;
                                 }
                             }
                             atomic_store(&storedFrames, at + frames);
                             atomic_fetch_add(&totalFrames, frames);
                         }
                               error:&err];
        if (!ok)
        {
            printf("RESULT: tap start failed: %s\n", err.localizedDescription.UTF8String);
            return 1;
        }
        printf("tap started: rate=%.0f ch=%u tappedPIDs=%s\n", tap.sampleRate, tap.channels,
               [tap.tappedPIDs componentsJoinedByString:@","].UTF8String ?: "(global)");
        fflush(stdout);

        uint64_t lastFrames = 0;
        double lastSum = 0;
        double maxRms = 0;
        for (int s = 1; s <= (int)seconds; s++)
        {
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];
            uint64_t tf = atomic_load(&totalFrames);
            double sum = sumSq;
            uint64_t df = tf - lastFrames;
            double rms = df ? sqrt((sum - lastSum) / (2.0 * df)) : 0;
            double dbfs = rms > 0 ? 20 * log10(rms) : -INFINITY;
            if (rms > maxRms) maxRms = rms;
            printf("t=%2ds frames=%llu cb=%u rms=%.5f (%.1f dBFS) peak=%.4f\n", s,
                   (unsigned long long)df, atomic_load(&callbacks), rms, dbfs, peak);
            fflush(stdout);
            lastFrames = tf;
            lastSum = sum;
        }
        [tap stop];
        size_t n = atomic_load(&storedFrames);
        if (n > maxFrames) n = maxFrames;
        WriteWav(wav, store, n, 2, (uint32_t)(tap.sampleRate > 0 ? tap.sampleRate : 48000));
        printf("RESULT: frames=%zu maxRms=%.5f peak=%.4f wav=%s %s\n", n, maxRms, peak,
               wav.UTF8String,
               peak > 0 ? "AUDIO CAPTURED" : "SILENT (TCC denied, or no audio playing)");
        free(store);
    }
    return 0;
}
