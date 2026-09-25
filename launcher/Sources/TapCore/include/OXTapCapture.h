// OXTapCapture — Core Audio process taps (macOS 14.2+) for the OXRSys launcher.
//
// Captures either a set of processes (e.g. the Wine game process) or all system
// audio as interleaved float32 PCM, optionally muting the tapped audio on the
// Mac while capturing. Must run inside an app bundle that has
// NSAudioCaptureUsageDescription and is launched by LaunchServices (not as a
// child of CrossOver) so the "System Audio Recording" TCC grant is attributed
// to the app itself.

#import <CoreAudio/CoreAudio.h>
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface OXAudioProcessInfo : NSObject
@property(nonatomic) pid_t pid;
@property(nonatomic) AudioObjectID objectID;
@property(nonatomic, copy, nullable) NSString* bundleID;
@property(nonatomic, copy) NSString* name; // from proc_name / argv[0] as best effort
@property(nonatomic) BOOL runningOutput;
@property(nonatomic) BOOL runningInput;
@end

// Called on the Core Audio IO thread (real-time). Do not block, allocate or log.
typedef void (^OXTapSampleBlock)(const float* interleaved, uint32_t frames,
                                 uint32_t channels, double sampleRate);

@interface OXTapCapture : NSObject

// All processes currently known to the Core Audio HAL.
+ (NSArray<OXAudioProcessInfo*>*)audioProcesses;

// Human-readable process name (full path-derived) for a PID.
+ (NSString*)nameForPID:(pid_t)pid;

// pids == nil or empty => all system audio (global tap). Otherwise a mixdown of
// the given processes; PIDs that Core Audio does not know yet are skipped (the
// process has not opened an audio client yet) and reported via error if none
// remain. mute => CATapMutedWhenTapped (Mac speakers silent while tapped).
- (BOOL)startWithPIDs:(nullable NSArray<NSNumber*>*)pids
                 mute:(BOOL)mute
          sampleBlock:(OXTapSampleBlock)block
                error:(NSError**)error;
- (void)stop;

@property(nonatomic, readonly) BOOL running;
@property(nonatomic, readonly) double sampleRate;
@property(nonatomic, readonly) uint32_t channels;
// PIDs actually included in the current tap (empty for global).
@property(nonatomic, readonly, copy) NSArray<NSNumber*>* tappedPIDs;

@end

NS_ASSUME_NONNULL_END
