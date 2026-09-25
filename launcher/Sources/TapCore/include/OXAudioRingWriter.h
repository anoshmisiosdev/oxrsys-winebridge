// OXAudioRingWriter — writer side of the OXAudioRing shared-memory PCM ring.
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface OXAudioRingWriter : NSObject
// Default path: ~/Library/Application Support/OXRSys/headset-audio.ring
+ (NSString*)defaultPath;
- (nullable instancetype)initWithPath:(NSString*)path
                           sampleRate:(uint32_t)rate
                             channels:(uint32_t)channels
                                scope:(uint32_t)scope
                                error:(NSError**)error;
// Real-time safe: memcpy + atomics only. Frames beyond capacity keep the newest.
- (void)writeInterleaved:(const float*)data frames:(uint32_t)frames;
// Marks the ring inactive (reader stops forwarding) and unmaps it.
- (void)close;
@property(nonatomic, readonly) uint64_t framesWritten;
@end

NS_ASSUME_NONNULL_END
