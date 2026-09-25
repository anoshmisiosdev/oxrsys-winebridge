// OXHeadsetAudio — process tap -> OXAudioRing glue. The tap IO block writes
// straight into the shared ring (no Swift, no allocation on the IO thread).
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(uint32_t, OXHeadsetAudioScope) {
    OXHeadsetAudioScopeGame = 1,
    OXHeadsetAudioScopeSystem = 2,
};

@interface OXHeadsetAudio : NSObject
// pids nil/empty => all system audio. Returns NO with error on failure.
- (BOOL)startWithPIDs:(nullable NSArray<NSNumber*>*)pids
                 mute:(BOOL)mute
                scope:(OXHeadsetAudioScope)scope
             ringPath:(nullable NSString*)ringPath
                error:(NSError**)error;
- (void)stop;
@property(nonatomic, readonly) BOOL running;
@property(nonatomic, readonly) double sampleRate;
@property(nonatomic, readonly, copy) NSArray<NSNumber*>* tappedPIDs;
@property(nonatomic, readonly) uint64_t framesWritten;
// Peak |sample| since the last call (for a level meter); resets on read.
- (float)takePeak;
@end

// Every process of the current user: @[ @[pid, argv0], ... ].
NSArray<NSArray*>* OXListProcesses(void);

NS_ASSUME_NONNULL_END
