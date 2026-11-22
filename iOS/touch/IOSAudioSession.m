/*
	The app's audio session: the game is heard with the ring/silent switch
	on silent, as a game the player started should be, and mixes with music
	or podcasts other apps are playing instead of stopping them.
*/

#import <AVFoundation/AVFoundation.h>

void IOSAudio_PlayWhenSilent(void)
{
  @autoreleasepool {
    AVAudioSession *session = [AVAudioSession sharedInstance];
    NSError *error = nil;
    if (![session setCategory:AVAudioSessionCategoryPlayback
                  withOptions:AVAudioSessionCategoryOptionMixWithOthers
                        error:&error])
      NSLog(@"UT2004: audio session category: %@", error);
    if (![session setActive:YES error:&error])
      NSLog(@"UT2004: audio session: %@", error);
  }
}
