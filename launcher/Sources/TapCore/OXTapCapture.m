#import "OXTapCapture.h"

#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#include <libproc.h>
#include <sys/sysctl.h>
#include <stdlib.h>
#include <string.h>

static NSError* OXError(NSString* what, OSStatus status)
{
    return [NSError errorWithDomain:@"OXTapCapture"
                               code:status
                           userInfo:@{NSLocalizedDescriptionKey :
                                          [NSString stringWithFormat:@"%@ (OSStatus %d)", what,
                                                                     (int)status]}];
}

@implementation OXAudioProcessInfo
@end

@implementation OXTapCapture
{
    AudioObjectID _tapID;
    AudioObjectID _aggregateID;
    AudioDeviceIOProcID _ioProcID;
    OXTapSampleBlock _block;
    float* _scratch; // interleave buffer for non-interleaved tap formats
    size_t _scratchFrames;
    BOOL _interleavedSource;
}

static OSStatus GetPropData(AudioObjectID obj, AudioObjectPropertySelector sel, UInt32* size,
                            void* data)
{
    AudioObjectPropertyAddress addr = {sel, kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    return AudioObjectGetPropertyData(obj, &addr, 0, NULL, size, data);
}

static UInt32 GetU32(AudioObjectID obj, AudioObjectPropertySelector sel)
{
    UInt32 v = 0, size = sizeof(v);
    GetPropData(obj, sel, &size, &v);
    return v;
}

+ (NSString*)nameForPID:(pid_t)pid
{
    char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
    // Wine rewrites argv[0] to the Windows exe path ("C:\...\HITMAN3.exe"); the
    // executable path is the wine preloader, so prefer the process args.
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
    size_t argmax = 0;
    int argmaxMib[2] = {CTL_KERN, KERN_ARGMAX};
    size_t sz = sizeof(argmax);
    if (sysctl(argmaxMib, 2, &argmax, &sz, NULL, 0) == 0 && argmax > 0)
    {
        char* buf = malloc(argmax);
        size_t len = argmax;
        if (buf && sysctl(mib, 3, buf, &len, NULL, 0) == 0 && len > sizeof(int))
        {
            // layout: int argc, exec path\0, padding \0..., argv[0]\0 ...
            char* p = buf + sizeof(int);
            char* end = buf + len;
            while (p < end && *p) p++;
            while (p < end && !*p) p++;
            if (p < end)
            {
                NSString* argv0 = [NSString stringWithUTF8String:p];
                free(buf);
                if (argv0.length) return argv0;
            }
        }
        free(buf);
    }
    if (proc_pidpath(pid, path, sizeof(path)) > 0) return [NSString stringWithUTF8String:path];
    char name[256] = {0};
    proc_name(pid, name, sizeof(name));
    return [NSString stringWithUTF8String:name];
}

+ (NSArray<OXAudioProcessInfo*>*)audioProcesses
{
    UInt32 size = 0;
    AudioObjectPropertyAddress addr = {kAudioHardwarePropertyProcessObjectList,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &size) != noErr)
        return @[];
    UInt32 count = size / sizeof(AudioObjectID);
    AudioObjectID* ids = calloc(count ? count : 1, sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, ids) != noErr)
    {
        free(ids);
        return @[];
    }
    count = size / sizeof(AudioObjectID);
    NSMutableArray* out = [NSMutableArray array];
    for (UInt32 i = 0; i < count; i++)
    {
        OXAudioProcessInfo* info = [OXAudioProcessInfo new];
        info.objectID = ids[i];
        pid_t pid = -1;
        UInt32 psz = sizeof(pid);
        GetPropData(ids[i], kAudioProcessPropertyPID, &psz, &pid);
        info.pid = pid;
        CFStringRef bid = NULL;
        UInt32 bsz = sizeof(bid);
        if (GetPropData(ids[i], kAudioProcessPropertyBundleID, &bsz, &bid) == noErr && bid)
            info.bundleID = CFBridgingRelease(bid);
        info.runningOutput = GetU32(ids[i], kAudioProcessPropertyIsRunningOutput) != 0;
        info.runningInput = GetU32(ids[i], kAudioProcessPropertyIsRunningInput) != 0;
        info.name = [self nameForPID:pid] ?: @"?";
        [out addObject:info];
    }
    free(ids);
    return out;
}

static AudioObjectID ProcessObjectForPID(pid_t pid)
{
    AudioObjectPropertyAddress addr = {kAudioHardwarePropertyTranslatePIDToProcessObject,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    AudioObjectID obj = kAudioObjectUnknown;
    UInt32 size = sizeof(obj);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, sizeof(pid), &pid, &size, &obj);
    return obj;
}

static NSString* DefaultOutputUID(void)
{
    AudioObjectID dev = kAudioObjectUnknown;
    UInt32 size = sizeof(dev);
    GetPropData(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultSystemOutputDevice, &size,
                &dev);
    if (dev == kAudioObjectUnknown) return nil;
    CFStringRef uid = NULL;
    size = sizeof(uid);
    if (GetPropData(dev, kAudioDevicePropertyDeviceUID, &size, &uid) != noErr || !uid) return nil;
    return CFBridgingRelease(uid);
}

- (BOOL)running { return _ioProcID != NULL; }

- (BOOL)startWithPIDs:(NSArray<NSNumber*>*)pids
                 mute:(BOOL)mute
          sampleBlock:(OXTapSampleBlock)block
                error:(NSError**)error
{
    [self stop];

    CATapDescription* desc = nil;
    NSMutableArray<NSNumber*>* tapped = [NSMutableArray array];
    if (pids.count == 0)
    {
        desc = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:@[]];
    }
    else
    {
        NSMutableArray<NSNumber*>* objs = [NSMutableArray array];
        for (NSNumber* pid in pids)
        {
            AudioObjectID obj = ProcessObjectForPID(pid.intValue);
            if (obj != kAudioObjectUnknown)
            {
                [objs addObject:@(obj)];
                [tapped addObject:pid];
            }
        }
        if (objs.count == 0)
        {
            if (error)
                *error = OXError(@"None of the requested processes has an audio client yet",
                                 kAudioHardwareBadObjectError);
            return NO;
        }
        desc = [[CATapDescription alloc] initStereoMixdownOfProcesses:objs];
    }
    desc.name = @"OXRSys headset audio";
    desc.privateTap = YES;
    desc.muteBehavior = mute ? CATapMutedWhenTapped : CATapUnmuted;
    NSUUID* tapUUID = [NSUUID UUID];
    desc.UUID = tapUUID;

    OSStatus st = AudioHardwareCreateProcessTap(desc, &_tapID);
    if (st != noErr)
    {
        _tapID = kAudioObjectUnknown;
        if (error) *error = OXError(@"AudioHardwareCreateProcessTap failed", st);
        return NO;
    }

    AudioStreamBasicDescription fmt = {0};
    UInt32 size = sizeof(fmt);
    st = GetPropData(_tapID, kAudioTapPropertyFormat, &size, &fmt);
    if (st != noErr)
    {
        if (error) *error = OXError(@"Reading tap format failed", st);
        [self stop];
        return NO;
    }
    _sampleRate = fmt.mSampleRate;
    _channels = fmt.mChannelsPerFrame ? fmt.mChannelsPerFrame : 2;
    _interleavedSource = (fmt.mFormatFlags & kAudioFormatFlagIsNonInterleaved) == 0;

    NSString* outUID = DefaultOutputUID();
    NSMutableDictionary* agg = [@{
        @kAudioAggregateDeviceNameKey : @"OXRSys Tap",
        @kAudioAggregateDeviceUIDKey : [NSUUID UUID].UUIDString,
        @kAudioAggregateDeviceIsPrivateKey : @YES,
        @kAudioAggregateDeviceIsStackedKey : @NO,
        @kAudioAggregateDeviceTapAutoStartKey : @YES,
        @kAudioAggregateDeviceTapListKey : @[ @{
            @kAudioSubTapDriftCompensationKey : @YES,
            @kAudioSubTapUIDKey : tapUUID.UUIDString,
        } ],
    } mutableCopy];
    if (outUID)
    {
        agg[@kAudioAggregateDeviceMainSubDeviceKey] = outUID;
        agg[@kAudioAggregateDeviceSubDeviceListKey] = @[ @{@kAudioSubDeviceUIDKey : outUID} ];
    }
    st = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)agg, &_aggregateID);
    if (st != noErr)
    {
        _aggregateID = kAudioObjectUnknown;
        if (error) *error = OXError(@"AudioHardwareCreateAggregateDevice failed", st);
        [self stop];
        return NO;
    }

    _block = [block copy];
    _scratchFrames = 8192;
    _scratch = calloc(_scratchFrames * _channels, sizeof(float));
    OXTapSampleBlock cb = _block;
    const uint32_t channels = _channels;
    const double rate = _sampleRate;
    const BOOL interleaved = _interleavedSource;
    float* scratch = _scratch;
    const size_t scratchFrames = _scratchFrames;

    st = AudioDeviceCreateIOProcIDWithBlock(
        &_ioProcID, _aggregateID, NULL,
        ^(const AudioTimeStamp* now, const AudioBufferList* inData, const AudioTimeStamp* inTime,
          AudioBufferList* outData, const AudioTimeStamp* outTime) {
            (void)now; (void)inTime; (void)outData; (void)outTime;
            if (!inData || inData->mNumberBuffers == 0) return;
            if (interleaved)
            {
                const AudioBuffer* b = &inData->mBuffers[0];
                uint32_t ch = b->mNumberChannels ? b->mNumberChannels : channels;
                uint32_t frames = b->mDataByteSize / (sizeof(float) * ch);
                if (b->mData && frames) cb((const float*)b->mData, frames, ch, rate);
            }
            else
            {
                uint32_t nb = inData->mNumberBuffers;
                uint32_t ch = nb < channels ? nb : channels;
                uint32_t frames = inData->mBuffers[0].mDataByteSize / sizeof(float);
                if (frames > scratchFrames) frames = (uint32_t)scratchFrames;
                for (uint32_t c = 0; c < ch; c++)
                {
                    const float* src = (const float*)inData->mBuffers[c].mData;
                    if (!src) return;
                    for (uint32_t f = 0; f < frames; f++) scratch[f * ch + c] = src[f];
                }
                if (frames) cb(scratch, frames, ch, rate);
            }
        });
    if (st != noErr)
    {
        _ioProcID = NULL;
        if (error) *error = OXError(@"AudioDeviceCreateIOProcIDWithBlock failed", st);
        [self stop];
        return NO;
    }
    st = AudioDeviceStart(_aggregateID, _ioProcID);
    if (st != noErr)
    {
        if (error) *error = OXError(@"AudioDeviceStart failed", st);
        [self stop];
        return NO;
    }
    _tappedPIDs = [tapped copy];
    return YES;
}

- (void)stop
{
    if (_aggregateID != kAudioObjectUnknown && _ioProcID)
    {
        AudioDeviceStop(_aggregateID, _ioProcID);
        AudioDeviceDestroyIOProcID(_aggregateID, _ioProcID);
    }
    _ioProcID = NULL;
    if (_aggregateID != kAudioObjectUnknown) AudioHardwareDestroyAggregateDevice(_aggregateID);
    _aggregateID = kAudioObjectUnknown;
    if (_tapID != kAudioObjectUnknown) AudioHardwareDestroyProcessTap(_tapID);
    _tapID = kAudioObjectUnknown;
    _block = nil;
    free(_scratch);
    _scratch = NULL;
    _tappedPIDs = @[];
}

- (void)dealloc { [self stop]; }

@end
