/*=============================================================================
	SDLLaunchPrivate.h: Unreal launcher for Linux.
	Copyright 1999-2002 Epic Games, Inc. All Rights Reserved.

Revision history:
	* Created by Daniel Vogel (based on XLaunch).
=============================================================================*/

#ifdef WIN32
#pragma pack(push,8)
#include <windows.h>
#pragma pack(pop)
#endif
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>

#if __APPLE__
#include <stdlib.h>
#else
#include <malloc.h>
#endif

#include <fcntl.h>
#include "Engine.h"
#include "UnRender.h"

int RunEngine( TCHAR* CmdLine );

#ifdef __ANDROID__
// SDLLaunchAndroid.cpp
void InitSplash(const TCHAR *filename);
bool AndroidFail(const char* Fmt, const char* Arg);
bool ExtractAssets(const char* Root);
bool InstallContent(const char* Root);
void StartServerService(const TCHAR* Cmd);
#elif __IOS__
// SDLLaunchIOS.cpp
void InitSplash(const TCHAR *filename);
bool IOSSetup();
void WaitWhileInBackground();
#elif __EMSCRIPTEN__
// SDLLaunchWeb.cpp
extern FOutputDevice* GWebLogHook;
#endif

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
