/*=============================================================================
	ALAudioCapture.h: Shared thread safe audio capture device.
	Copyright 2003 Epic Games, Inc. All Rights Reserved.

	Revision history:
	* Created by Daniel Vogel.
=============================================================================*/

#ifndef _INC_ALAUDIOCAPTURE
#define _INC_ALAUDIOCAPTURE

/*------------------------------------------------------------------------------------
	Dependencies.
------------------------------------------------------------------------------------*/

#include "ALAudio.h"

// OpenAL is loaded at runtime through ALFuncs.h, only its types and constants come from the headers.
// Where it is linked in, ALAudioSubsystem.h takes the prototypes, so they must not be switched off here.
#if !__EMSCRIPTEN__ && !__IOS__
#define AL_NO_PROTOTYPES
#define ALC_NO_PROTOTYPES
#endif
#include "AL/al.h"
#include "AL/alc.h"

/*------------------------------------------------------------------------------------
	FALAudioCaptureDevice.
------------------------------------------------------------------------------------*/

#define FILTER_8K_SIZE 11

class FALAudioCaptureDevice : public FAudioCaptureDevice
{
public:
	FALAudioCaptureDevice();
	~FALAudioCaptureDevice();

	virtual UBOOL Init();
	virtual void Record();
	virtual void Stop();

	virtual FLOAT GetSampleData( DWORD& Offset, void* Pointer, DWORD& Samples, EAudioCaptureType Type );
	virtual DWORD GetCurrentOffset();
	virtual	DWORD GetAvailableSamples( DWORD Offset, EAudioCaptureType Type );
	virtual DWORD GetSamplesPerSecond();

protected:
	// ALC_EXT_capture support...
	UBOOL						HaveALCaptureExt;
	ALCdevice*					ALCaptureDevice;
	BYTE*						ALCaptureBuffer;
	ALenum						alc_capture_samples;
 
	FCriticalSection			CriticalSection;
	INT							RecordCount;

	static	FLOAT				FilterNumerator[FILTER_8K_SIZE];
	static	FLOAT				FilterDenominator[FILTER_8K_SIZE];
	FLOAT						FilterMemory[FILTER_8K_SIZE-1];
};

#endif

