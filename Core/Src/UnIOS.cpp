// iOS: waits on the main thread run its run loop instead of sleeping. UIKit hands touches over
// only when it sees the main thread idle in the run loop, so a thread that is only ever working
// or asleep gets its touches seconds late. Kept apart from the engine headers, whose macros
// clash with the Apple ones.

#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <time.h>

static double NowSeconds()
{
	timespec Now;
	clock_gettime( CLOCK_MONOTONIC, &Now );
	return Now.tv_sec + Now.tv_nsec / 1000000000.0;
}

// Waits Seconds. On the main thread the run loop runs until 0.2 ms before the end, then a short
// sleep takes the rest, so the wait stays as precise as a sleep. Other threads just sleep.
extern "C" void appIOSWait( double Seconds )
{
	if( Seconds <= 0.0 )
		return;
	double End = NowSeconds() + Seconds;
	if( pthread_main_np() )
	{
		for( double Left = End - NowSeconds(); Left > 0.0002; Left = End - NowSeconds() )
			CFRunLoopRunInMode( kCFRunLoopDefaultMode, Left - 0.0002, true );
	}
	double Left = End - NowSeconds();
	if( Left > 0.0 )
	{
		timespec Rest = { (time_t)Left, (long)((Left - (time_t)Left) * 1000000000.0) };
		nanosleep( &Rest, NULL );
	}
}
