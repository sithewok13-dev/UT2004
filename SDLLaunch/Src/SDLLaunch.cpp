/*=============================================================================
	SDLLaunch.cpp: Game launcher.
	Copyright 2002 Epic Games, Inc. All Rights Reserved.

Revision history:
	* Created by Ryan C. Gordon, based on ut2003's win32 Launch.cpp.
=============================================================================*/

#include <time.h>  // dodging a macro clash with Unix C runtime headers...

#if !__EMSCRIPTEN__
// Before the engine headers, whose operator delete the standard library headers would redeclare.
#include "SDLLaunchContent.h"
#endif
#include "SDLLaunchPrivate.h"

#if __STATIC_LINK
#include "SDLDrv.h"
#include "OpenGLDrv.h"
#include "ALAudio.h"
#include "StaticLink.h"
extern "C" BYTE GLoadedSDLDrv, GLoadedOpenGLDrv, GLoadedALAudio;

#endif


/*-----------------------------------------------------------------------------
	Global variables.
-----------------------------------------------------------------------------*/

// General.
#ifdef WIN32
extern "C" {HINSTANCE hInstance;}
#endif
extern "C" {TCHAR GPackage[64]=TEXT("SDLLaunch");}

#if __STATIC_LINK && defined(_MSC_VER)
// Statically linked third-party libraries allocate in their static constructors, and the engine's global operator new routes
// that through GMalloc. This file's globals initialize in the library phase before theirs, EarlyMalloc sets GMalloc there.
#pragma init_seg(lib)
#endif
// Memory allocator.
#ifdef _DEBUG
	#include "FMallocDebug.h"
	FMallocDebug Malloc;
#else
	#include "FMallocAnsi.h"
	FMallocAnsi Malloc;
#endif

#if __STATIC_LINK
static FEarlyMalloc EarlyMalloc EARLY_INIT_PRIORITY = &Malloc;
#endif

// Log file.
#include "FOutputDeviceFile.h"
FOutputDeviceFile Log;

// Error handler.
#include "FOutputDeviceAnsiError.h"
FOutputDeviceAnsiError Error;

// Feedback.
#include "FFeedbackContextAnsi.h"
FFeedbackContextAnsi Warn;

// File manager.
#ifdef WIN32
#include "FFileManagerWindows.h"
FFileManagerWindowsHome FileManager;
#else
#include "FFileManagerUnix.h"
FFileManagerUnix FileManager;
#endif

// Config.
#include "FConfigCacheIni.h"

/*-----------------------------------------------------------------------------
	mainline
-----------------------------------------------------------------------------*/

// SDL3.dll uses default packing, the engine builds with /Zp4.
#pragma pack(push,8)
#include <SDL3/SDL.h>
#pragma pack(pop)

#if defined(__ANDROID__) || __IOS__
static void ExitSplash(void) {}
#elif __EMSCRIPTEN__
// The page shows the loading progress, and a 2D context would keep SDLDrv from getting WebGL on the same canvas.
static void InitSplash(const TCHAR *filename) {}
static void ExitSplash(void) {}
#else
static SDL_Window* SplashWindow = NULL;

static void InitSplash(const TCHAR *filename)
{
    if (filename == NULL)
        return;

    // A GL backed surface would make its context current and take that away from SDLDrv's.
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "0");
    if (!SDL_Init(SDL_INIT_VIDEO))
        return;

    SDL_Surface *bmp = SDL_LoadBMP(appToAnsi(filename));
    if (bmp == NULL)
        return;

    SplashWindow = SDL_CreateWindow("", bmp->w, bmp->h, SDL_WINDOW_BORDERLESS);
    SDL_Surface *screen = SplashWindow ? SDL_GetWindowSurface(SplashWindow) : NULL;
    if (screen != NULL)
    {
        SDL_BlitSurface(bmp, NULL, screen, NULL);
        SDL_UpdateWindowSurface(SplashWindow);
    }

    SDL_DestroySurface(bmp);
}


static void ExitSplash(void)
{
    if (SplashWindow == NULL)
        return;
    SDL_DestroyWindow(SplashWindow);
    SplashWindow = NULL;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}
#endif

#if defined(__ANDROID__) || __IOS__ || __EMSCRIPTEN__
static void InitServerTray(void) {}
static void PumpServerTray(void) {}
static void ExitServerTray(void) {}
static bool FindContent(void) { return true; }
#else
// A dedicated server has no window, the tray icon is where it can be seen and stopped.
// It lives on its own thread, because Windows blocks the thread that pumps the tray for as long as its menu is open.
static SDL_Thread* TrayThread = NULL;
static SDL_AtomicInt TrayShowLog, TrayStop, TrayQuit;

// The menu entries only raise a flag, the engine is not safe to touch from the tray thread.
static void SDLCALL RaiseTrayFlag(void* Flag, SDL_TrayEntry*)
{
    SDL_SetAtomicInt((SDL_AtomicInt*)Flag, 1);
}

static int SDLCALL RunServerTray(void*)
{
    if (!SDL_Init(SDL_INIT_VIDEO))
        return 0;
    // SDL keeps the display awake for games, a server in the tray has no reason to.
    SDL_EnableScreenSaver();
    SDL_Surface *icon = SDL_LoadBMP("../Help/Unreal.bmp");
    if (icon != NULL)
        SDL_SetSurfaceColorKey(icon, true, SDL_MapSurfaceRGB(icon, 255, 0, 255));
    SDL_Tray *tray = SDL_CreateTray(icon, "UT2004 Server");
    SDL_DestroySurface(icon);
    if (tray == NULL)
        return 0;
    SDL_TrayMenu *menu = SDL_CreateTrayMenu(tray);
    SDL_SetTrayEntryCallback(SDL_InsertTrayEntryAt(menu, -1, "Show log", SDL_TRAYENTRY_BUTTON), RaiseTrayFlag, &TrayShowLog);
    SDL_SetTrayEntryCallback(SDL_InsertTrayEntryAt(menu, -1, "Stop server", SDL_TRAYENTRY_BUTTON), RaiseTrayFlag, &TrayStop);
    // SDL runs the tray callbacks while it pumps events, the events themselves are of no use here.
    while (!SDL_GetAtomicInt(&TrayQuit))
    {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {}
        SDL_Delay(50);
    }
    SDL_DestroyTray(tray);
    return 0;
}

static void InitServerTray(void)
{
    TrayThread = SDL_CreateThread(RunServerTray, "ServerTray", NULL);
}

static void PumpServerTray(void)
{
    if (SDL_CompareAndSwapAtomicInt(&TrayShowLog, 1, 0))
    {
        // The file writer holds back up to 4 KB, which can be the whole log of a quiet server.
        Log.Flush();
#ifdef WIN32
        FString Path = Log.Filename;
#else
        FString Path = Log.Filename[0] == '/' ? FString(Log.Filename) : FString(GFileManager->CalcHomeDir()) + FString(Log.Filename);
#endif
        SDL_OpenURL(appToAnsi(*Path));
    }
    if (SDL_GetAtomicInt(&TrayStop))
        appRequestExit(0);
}

static void ExitServerTray(void)
{
    SDL_SetAtomicInt(&TrayQuit, 1);
    SDL_WaitThread(TrayThread, NULL);
}

// SDL may answer on another thread, so the result crosses over without the engine's allocator.
struct FFolderPick
{
    char Path[1024];
    bool Failed;
    SDL_AtomicInt Done;
};

static void SDLCALL FolderPicked(void* UserData, const char* const* List, int)
{
    FFolderPick* Pick = (FFolderPick*)UserData;
    // SDL keeps its error text per thread, so it is taken here.
    Pick->Failed = List == NULL;
    SDL_strlcpy(Pick->Path, Pick->Failed ? SDL_GetError() : List[0] ? List[0] : "", sizeof(Pick->Path));
    SDL_SetAtomicInt(&Pick->Done, 1);
}

// Returns the picked folder, empty when the player backs out or the system has no folder dialog.
static FString BrowseContent(void)
{
    FFolderPick Pick = {};
    SDL_ShowOpenFolderDialog(FolderPicked, &Pick, NULL, NULL, false);
    while (!SDL_GetAtomicInt(&Pick.Done))
    {
        SDL_PumpEvents();
        SDL_Delay(10);
    }
    if (Pick.Failed)
    {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "UT2004 could not ask for the folder", Pick.Path, NULL);
        return TEXT("");
    }
    return appFromUtf8(Pick.Path);
}

// The engine joins the base directory and each path in 256 character buffers.
static bool ContentFolderFits(const FString& Folder)
{
    if (appStrlen(appBaseDir()) + Folder.Len() + 24 <= 255)
        return true;
    FString Message = FString::Printf(TEXT("The path of this folder is too long for the game:\n%s"), *Folder);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "UT2004", &appToUtf8(*Message)(0), NULL);
    return false;
}

static void DownloadContent(void)
{
    // Beside the settings, so it is the game folder unless that one is read-only. The default paths look there.
    FString Folder = GFileManager->CalcHomeDir();
    INT End = Folder.Len() - 1;
    while (End > 0 && Folder[End - 1] != '/' && Folder[End - 1] != '\\')
        End--;
    TArray<ANSICHAR> Root = appToUtf8(*Folder.Left(Max(End - 1, 0)));
    FInstall I = {};
    I.Root = &Root(0);
    I.Src = UT_FULL_URL;
    bool Ok = InstallFull(I);
    CloseSetupScreen();
    if (!Ok && !SetupQuit)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "UT2004", I.Err, NULL);
}

static bool ContentFound(void)
{
    for (INT i = 0; i < GSys->Paths.Num(); i++)
        if (GSys->Paths(i).Right(4) == TEXT(".ut2") && GFileManager->FindFiles(*GSys->Paths(i), 1, 0).Num())
            return true;
    return false;
}

// The build ships without the game content, so the player downloads it or points at a retail install, which is used in place.
// Returns false when the player gives up.
static bool FindContent(void)
{
    if (ContentFound())
        return true;

    static const TCHAR* Wildcards[] = { TEXT("Maps/*.ut2"), TEXT("Textures/*.utx"), TEXT("Sounds/*.uax"), TEXT("Music/*.umx"), TEXT("StaticMeshes/*.usx"), TEXT("Animations/*.ukx") };
    // Without video there is no dialog to ask with, and the engine reports that failure itself.
    if (!SDL_Init(SDL_INIT_VIDEO))
        return true;
    enum { Quit, Download, Browse };
    static const SDL_MessageBoxButtonData Buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, Quit, "Quit" },
        { 0, Browse, "Browse..." },
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, Download, "Download" },
    };
    static const SDL_MessageBoxData Box = { SDL_MESSAGEBOX_INFORMATION, NULL, "UT2004",
        "The game content was not found.\n\n"
        "Download it from the Internet Archive (3.8 GB, needs 13 GB of free space), "
        "or browse to the folder of a UT2004 install, the one that holds Maps and Textures.",
        SDL_arraysize(Buttons), Buttons, NULL };
    bool Found = false;
    while (!Found && !SetupQuit)
    {
        int Choice = Quit;
        if (!SDL_ShowMessageBox(&Box, &Choice) || Choice == Quit)
            break;
        if (Choice == Download)
        {
            DownloadContent();
            Found = ContentFound();
            continue;
        }
        FString Folder = BrowseContent();
        if (!Folder.Len() || !ContentFolderFits(Folder) || !GFileManager->FindFiles(*(Folder * Wildcards[0]), 1, 0).Num())
            continue;
        for (const TCHAR* Wildcard : Wildcards)
            new(GSys->Paths) FString(Folder * Wildcard);
        new(GSys->MusicPath) FString(Folder * TEXT("Music"));
        GSys->SaveConfig();
        Found = true;
    }
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return Found;
}
#endif


#if __EMSCRIPTEN__
#include <unistd.h>
// Emscripten/content.js
extern "C" int ut_next_frame();
extern "C" void ut_exit(const char* ServerCmd);
#endif

#if defined(__ANDROID__) || __IOS__
#pragma pack(push,8)
#include <SDL3/SDL_main.h>
#pragma pack(pop)
#include <unistd.h>
#endif


//
// Unreal's main message loop.  All windows in Unreal receive messages
// somewhere below this function on the stack.
//

class CMainLoop
{
public:
	inline CMainLoop( UEngine* InEngine, UBOOL InIncludeMessaging = true );
	inline ~CMainLoop(void) noexcept(false);

	inline void	RunLoop(void);
	inline bool	Finished(void) const	{return !GIsRunning || GIsRequestingExit;}

private:
	UEngine *		Engine;
	UBOOL			IncludeMessaging;

	DOUBLE			OldTime;
	DOUBLE			SecondStartTime;
	INT				TickCount;
	DWORD			LastFrameCycles;

	// Benchmarking.
	INT				BMFrames;
	INT				BMDiscardedFrames;
	INT				BMMaxFrames;
	FLOAT			BMSeconds;
	TArray<FString> BMStrings;
	TArray<FLOAT>	BMFrameTimes;

	// Movie recording.
	UBOOL			RecordingMovie;

};

inline CMainLoop::CMainLoop( UEngine* InEngine, UBOOL InIncludeMessaging )
:	Engine(InEngine), IncludeMessaging(InIncludeMessaging)
{
	guard(MainLoopCtor);
	check(Engine);

	// Loop while running.
	GIsRunning			= 1;
	OldTime				= appSeconds();
	SecondStartTime		= OldTime;
	TickCount			= 0;

	// Benchmarking.
	BMFrames			= 0;
	BMDiscardedFrames	= 10;
	BMSeconds			= 0.0f;
	
	BMMaxFrames			= 0; // gam
	Parse(appCmdLine(),TEXT("SECONDS="),BMMaxFrames);
	BMMaxFrames			*= 30;
	
	for( INT i=0; i<(BMMaxFrames-BMDiscardedFrames+1); i++ )
		new(BMStrings) FString(TEXT(""));
	if( BMMaxFrames )
		BMFrameTimes.Add(BMMaxFrames-BMDiscardedFrames+1);

	LastFrameCycles		= appCycles();

	// Seed random number generator with constant seed for benchmarking.
	if( GIsBenchmarking )
		appRandInit( 0 );

	// Movie recording.
	RecordingMovie	= ParseParam(appCmdLine(),TEXT("RECORDMOVIE"));
		
	unguard;
}

inline void	CMainLoop::RunLoop(void)
{
	// Ever really needed ? - Erik
	if( Finished() ) 
		return;

#if __IOS__
	WaitWhileInBackground();
	if( Finished() )
		return;
#endif

	// Clear stats (will also update old stats).
	GStats.Clear();

    if( GIsBenchmarking )
	{
		if( BMFrames == BMDiscardedFrames )
			BMSeconds = 0.0f;
		else if( BMFrames > BMMaxFrames )
			appRequestExit(0);
	}

	// Update the world.
	DOUBLE NewTime   = appSeconds();
	FLOAT  DeltaTime = NewTime - OldTime;

	guard(UpdateWorld);
	if( GUseFixedTimeStep )
	{
		GDeltaTime = GFixedTimeStep;
		GCurrentTime += GFixedTimeStep;
		if( BMFrames < BMMaxFrames )
			BMSeconds += DeltaTime;
		BMFrames++;
	}
	else
	{
		GCurrentTime = NewTime;
		GDeltaTime = DeltaTime;
	}

	//Update.
	if( GDeltaTime < 0.f )
		GDeltaTime = 1 / 20.f;
	Engine->Tick( GDeltaTime );
	// sjs --- engine::tick may load a new map and cause the timing to be reset (this is a good thing)
	if( appSeconds() < NewTime )
           SecondStartTime = NewTime = GCurrentTime = appSeconds();
	// --- sjs

	TickCount++;
	OldTime = NewTime;

	if( OldTime > SecondStartTime + 1 )
	{
		if( GUseFixedTimeStep )
			Engine->CurrentTickRate = 1 / GFixedTimeStep;
		else
			Engine->CurrentTickRate = (FLOAT)TickCount / (OldTime - SecondStartTime);
		SecondStartTime = OldTime;
		TickCount = 0;
	}
	unguard;

	// Enforce optional maximum tick rate.
	guard(EnforceTickRate);
#if __EMSCRIPTEN__
	// The browser shows the frame and runs its events only while the game waits for the next one.
	// It also tells when the page's Stop server button was pressed.
	if( ut_next_frame() )
		appRequestExit( 0 );
#else
	if( !GUseFixedTimeStep && IncludeMessaging )
	{		
		FLOAT MaxTickRate = Engine->GetMaxTickRate();
		// Nobody sees a minimized or covered window, so it does not need more frames than the game needs ticks.
		SDL_Window* Window = SDL_GL_GetCurrentWindow();
		if( Window && (SDL_GetWindowFlags( Window ) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED)) )
			MaxTickRate = MaxTickRate > 0.f ? Min( MaxTickRate, 30.f ) : 30.f;
		if( MaxTickRate>0.0 )
		{
			// Sleep alone wakes up to a millisecond late.
			DOUBLE Delta = 1.0 / MaxTickRate - (appSeconds() - OldTime);
			if( Delta > 0.0 )
				SDL_DelayPrecise( (Uint64)(Delta * 1000000000.0) );
		}
	}
#endif
	unguard;

	if( IncludeMessaging && Engine->Client )
	{
		// Handle all incoming messages.
		guard(MessagePump);
		Engine->Client->Viewports(0)->UpdateInput(0, 0);
		unguard;
	}
	else if( !Engine->Client )
		PumpServerTray();

	if( RecordingMovie && Engine->Client )
		Engine->Client->Viewports(0)->Exec(TEXT("shot"));

	GStats.DWORDStats( GEngineStats.STATS_Frame_TotalCycles ) = appCycles() - LastFrameCycles;
	GStats.DWORDStats( GEngineStats.STATS_Game_ScriptCycles ) = GScriptCycles;
	GScriptCycles	= 0;
	LastFrameCycles = appCycles();

	if( GIsBenchmarking )
	{
		if( BMFrames == 1 )
		{
			// Get descriptions.
			GStats.UpdateString( BMStrings(0), 1 );
			BMFrameTimes(0) = 0.f;
		}
		else
		{
			// Get frame stats. (Index already implies the +1 for descriptions)
			INT BMStringsIndex = Clamp( BMFrames - BMDiscardedFrames, 2, BMMaxFrames - BMDiscardedFrames );
			GStats.UpdateString( BMStrings(BMStringsIndex - 1), 0 );
			BMFrameTimes( BMStringsIndex - 1 ) = GStats.DWORDStats( GEngineStats.STATS_Frame_TotalCycles ) * GSecondsPerCycle * 1000.f;
		}
	}
}

inline CMainLoop::~CMainLoop(void) noexcept(false)
{
	guard(CMainLoopDtor);

	GIsRunning = 0;
	if( GIsBenchmarking )
	{
		INT Frames		= BMMaxFrames - BMDiscardedFrames; 
		UBOOL LogLowFPS = ParseParam(appCmdLine(),TEXT("ONLYLOGLOWFPS"));
		FLOAT LowFPS	= 100.f; // in ms

		// Level.
		FString LevelName = Engine->Client->Viewports(0)->Actor->GetViewTarget()->XLevel->GetPathName();
		LevelName = LevelName.LeftChop( 8 );

		// Get time & date.
		INT Year, Month, DayOfWeek, Day, Hour, Minutes, Sec, MSec;
		appSystemTime( Year, Month, DayOfWeek, Day, Hour, Minutes, Sec, MSec );
		FString DateTime = FString::Printf(TEXT("%i-%02i-%02i-%02i-%02i-%02i"),Year,Month,Day,Hour,Minutes,Sec);

		// Machine Details.
		FString MachineString = FString::Printf(TEXT("%s\r\n%s\r\n%s\r\n%s\r\n\r\n%s\r\n\r\n"),GBuildLabel,GMachineOS,GMachineCPU,GMachineVideo,appCmdLine());
		FString OutputString = MachineString;
		FString LowFPSString = TEXT("");

		// Count how many frames take more than LowFPS ms.
		if( LogLowFPS )
		{
			INT Count = 0;
			for( INT i=1; i<Frames; i++ )	
				if( BMFrameTimes(i) > LowFPS )
					Count++;
			
			LowFPSString = FString::Printf(TEXT("High frametimes: %i / %i == %f percent \r\n\r\n"), Count, Frames, 100.f * FLOAT(Count) / Frames ); 
			OutputString += LowFPSString;
		}

		// Determine min/ max framerate and score.
		FLOAT	MinFPS = 1000.f,
				MaxFPS = 0.f,
				AvgFPS = Frames / BMSeconds,
				Score  = 0.f;

		// Check for FRAMETIMECAP command line option.
		FLOAT FPSCap = 0;
		Parse(appCmdLine(),TEXT("MAXFPS="),FPSCap);
		if( FPSCap <= 0 )
			FPSCap = 80;
		FLOAT FrameTimeCap = 1000.f / FPSCap;

		for( INT i=1; i<Frames; i++ )
		{
			// Calculate min/max.
			FLOAT FPS = 1000.f / BMFrameTimes(i);
			MinFPS = Min( MinFPS, FPS );
			MaxFPS = Max( MaxFPS, FPS );

			// Calculate score.
			Score += Max( BMFrameTimes(i), FrameTimeCap ) ;
		}

		Score = Frames / Score * 1000.f;

		// Output detailed results to a file.
		INT LastRand = appRand();
		for( INT i=0; i<Frames; i++ )
		{
			if( LogLowFPS && i!=0 )
			{
				if( BMFrameTimes(i) > LowFPS )
					OutputString += BMStrings(i);
			}
			else
				OutputString += BMStrings(i);
		}
		TCHAR File[1024];
		appSprintf( File, TEXT("..\\Benchmark\\CSVs\\stats-%s.csv"), *DateTime );
		appSaveStringToFile( OutputString, File );

		// Output average framerate.
		OutputString = TEXT("");
		appLoadFileToString( OutputString, TEXT("..\\Benchmark\\benchmark.log") );
		OutputString += FString::Printf(TEXT("%f / %f / %f fps -- Score = %f        rand[%i]\r\n"), MinFPS, AvgFPS, MaxFPS, Score, LastRand );
		appSaveStringToFile( OutputString, TEXT("..\\Benchmark\\benchmark.log") );
		OutputString = MachineString;
		if( LogLowFPS )
			OutputString += LowFPSString;
		OutputString += FString::Printf(TEXT("%f / %f / %f fps         rand[%i]\r\nScore = %f\r\n"), MinFPS, AvgFPS, MaxFPS, LastRand, Score );
		appSaveStringToFile( OutputString, *FString::Printf(TEXT("..\\Benchmark\\Results\\avgfps-%s.log"), *DateTime ) );
	
		// Output low framerate stats.
		OutputString = TEXT("");
		appLoadFileToString( OutputString, TEXT("..\\Benchmark\\lowframerate.log") );
		OutputString += FString::Printf(TEXT("%s\r\n%f / %f / %f fps\r\nScore = %f\r\n%s\r\n"), *LevelName, MinFPS, AvgFPS, MaxFPS, Score, *LowFPSString );
		appSaveStringToFile( OutputString, TEXT("..\\Benchmark\\lowframerate.log") );

		GLog->Flush();
		GFileManager->Copy( *FString::Printf(TEXT("..\\Benchmark\\Logs\\ut2004-%s.log"),*DateTime), TEXT("ut2004.log") );
	}

	unguard;
}

static void MainLoop( UEngine* Engine )
{
	guard(MainLoop);
	check(Engine);

	CMainLoop* theLoop = new CMainLoop(Engine);
	while (!theLoop->Finished()) 
	{
		theLoop->RunLoop();
	}
	
	delete theLoop;

	unguard;
}



static UEngine* InitEngine()
{
	guard(InitEngine);
	DOUBLE LoadTime = appSeconds();

	// Set exec hook.
	GExec = NULL;

	// First-run menu.
	INT FirstRun=0;
	GConfig->GetInt( TEXT("FirstRun"), TEXT("FirstRun"), FirstRun );

	// Older inis and the retail menu still name drivers that no longer exist.
	FString Viewport = GConfig->GetStr(TEXT("Engine.Engine"),TEXT("ViewportManager"),TEXT("System"));
	if( Viewport!=TEXT("SDLDrv.SDLClient") )
	{
		debugf(TEXT("Forcing use of SDLDrv instead of %s."),*Viewport);
		GConfig->SetString(TEXT("Engine.Engine"), TEXT("ViewportManager"), TEXT("SDLDrv.SDLClient"));
	}
	FString Renderer = GConfig->GetStr(TEXT("Engine.Engine"),TEXT("RenderDevice"),TEXT("System"));
	if( Renderer!=TEXT("OpenGLDrv.OpenGLRenderDevice") && Renderer!=TEXT("Engine.NullRenderDevice") )
	{
		debugf(TEXT("Forcing use of OpenGLDrv instead of %s."),*Renderer);
		GConfig->SetString(TEXT("Engine.Engine"), TEXT("RenderDevice"), TEXT("OpenGLDrv.OpenGLRenderDevice"));
	}
	// Older inis name a controller class that no longer exists, and lack the page that replaced it.
	if( GConfig->GetStr(TEXT("Engine.Engine"),TEXT("GUIController"),TEXT("System"))==TEXT("GUI2K4Fix.FixGUIController") )
	{
		debugf(TEXT("Replacing the removed GUI2K4Fix.FixGUIController."));
		GConfig->SetString(TEXT("Engine.Engine"), TEXT("GUIController"), TEXT("GUI2K4.UT2K4GUIController"));
		FString AutoLoad;
		// An ini entry replaces the whole default list, so the chat page goes in with it.
		if( !GConfig->GetString(TEXT("GUI2K4.UT2K4GUIController"), TEXT("AutoLoad"), AutoLoad, GUserIni) )
		{
			const TCHAR* Menus[] = { TEXT("GUI2K4.UT2K4InGameChat"), TEXT("GUI2K4Fix.FixMenuSetup") };
			for( INT i=0; i<ARRAY_COUNT(Menus); i++ )
				GConfig->SetString(TEXT("GUI2K4.UT2K4GUIController"), TEXT("AutoLoad"), *FString::Printf(TEXT("(MenuClassName=\"%s\",bPreInitialize=True)"), Menus[i]), GUserIni, 0);
		}
	}

	// EXEC from command-line
	FString Command;
	if( Parse(appCmdLine(),TEXT("consolecommand="), Command) )
	{
        if (GExec == NULL)
        {
            debugf(TEXT("Can't use consolecommand"));
        }
        else
        {
		    debugf(TEXT("Executing console command %s"),*Command);
    		GExec->Exec( *Command, *GLog );
        }
		return NULL;
	}

	// Update first-run.
	if( FirstRun<ENGINE_VERSION )
		FirstRun = ENGINE_VERSION;
	GConfig->SetInt( TEXT("FirstRun"), TEXT("FirstRun"), FirstRun );


	// Create the global engine object.
	UClass* EngineClass;
	if( !GIsEditor )
	{
		// Create game engine.
		EngineClass = UObject::StaticLoadClass( UGameEngine::StaticClass(), NULL, TEXT("ini:Engine.Engine.GameEngine"), NULL, LOAD_NoFail, NULL );
	}
	else
	{
		// Editor.
		EngineClass = UObject::StaticLoadClass( UEngine::StaticClass(), NULL, TEXT("ini:Engine.Engine.EditorEngine"), NULL, LOAD_NoFail, NULL );
	}
	UEngine* Engine = ConstructObject<UEngine>( EngineClass );
	Engine->Init();
	debugf( TEXT("Startup time: %f seconds"), appSeconds()-LoadTime );

	return Engine;
	unguard;
}



//
// Main entry point.
// This is an example of how to initialize and launch the engine.
//
int main(int argc, char **argv)
{
#ifdef __ANDROID__
	SDL_SetHint( SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight" );
	// Back opens and closes menus like Escape instead of closing the app.
	SDL_SetHint( SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1" );
	// Mouse clicks would arrive as fingers too and bring the on-screen controls back.
	SDL_SetHint( SDL_HINT_MOUSE_TOUCH_EVENTS, "0" );
	// App specific storage needs no permission, and players can still reach the inis over USB.
	const char* Root = SDL_GetAndroidExternalStoragePath();
	if( !Root )
		Root = SDL_GetAndroidInternalStoragePath();
	bool Installed = ExtractAssets(Root) && InstallContent(Root);
	CloseSetupScreen();
	if( !Installed )
		return 1;
	// Apps have no usable HOME, which is where the download cache would go otherwise.
	if( const char* Cache = SDL_GetAndroidCachePath() )
		setenv( "XDG_CACHE_HOME", Cache, 1 );
	// The engine runs from System/ like on the desktop.
	char SystemDir[1024];
	SDL_snprintf( SystemDir, sizeof(SystemDir), "%s/UT2004/System", Root );
	if( chdir(SystemDir) != 0 )
		return AndroidFail( "Couldn't open %s.", SystemDir ), 1;
#elif __IOS__
	SDL_SetHint( SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight" );
	// Mouse clicks would arrive as fingers too and bring the on-screen controls back.
	SDL_SetHint( SDL_HINT_MOUSE_TOUCH_EVENTS, "0" );
	// The setup and splash screens draw with OpenGL ES, as SDLDrv puts its GL context on the same window afterwards.
	SDL_SetHint( SDL_HINT_RENDER_DRIVER, "opengles2" );
	// A swipe from the bottom edge first only shows the home indicator, so the game isn't left by accident.
	SDL_SetHint( SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2" );
	if( !IOSSetup() )
		return 1;
#elif __EMSCRIPTEN__
	// The system package and Emscripten/index.html lay the files out like a build folder.
	chdir( "/System" );
	// Keys reach the game only while the canvas has focus, so typing elsewhere on the page still works.
	SDL_SetHint( SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT, "#canvas" );
	// SDL would yield on every swap under JSPI, the main loop already waits for each animation frame.
	SDL_SetHint( SDL_HINT_EMSCRIPTEN_ASYNCIFY, "0" );
#endif
	TCHAR CmdLine[1024] = TEXT("");
#ifdef WIN32
	// appInit skips the executable name on Windows, as GetCommandLine starts with it.
	appStrncpy( CmdLine, GetCommandLine(), ARRAY_COUNT(CmdLine) );
#else
    if (argc >= 2)
    {
	    appStrncpy(CmdLine, appFromAnsi(argv[1]), ARRAY_COUNT(CmdLine));
	    for (INT i = 2; i < argc && appStrlen(CmdLine) + strlen(argv[i]) + 1 < ARRAY_COUNT(CmdLine); i++)
    	{
       		appStrcat(CmdLine, TEXT(" "));
    		appStrcat(CmdLine, appFromAnsi(argv[i]));
    	}
    }
#endif
#if __IOS__
	// The home screen starts apps without arguments. Android's activity passes the same ones.
	if( !CmdLine[0] )
		appStrcpy( CmdLine, TEXT("-nohomedir -VIRTUALJOYSTICK") );
#endif
	return RunEngine( CmdLine );
}

int RunEngine( TCHAR* CmdLine )
{
	// Remember instance.
	INT ErrorLevel = 0;
	GIsStarted     = 1;

#ifdef WIN32
	hInstance = GetModuleHandle( NULL );
#endif
	strcpy( GModule, appToAnsi(TEXT("UT2004")) );
	appStrcpy( GPackage, appPackage() );

	// Begin guarded code.
#ifndef _DEBUG
	try
	{
#endif

		// Init core.
		GIsClient = GIsGuarded = 1;
#if __EMSCRIPTEN__
		GLogHook = GWebLogHook;
#endif
		appInit( GPackage, CmdLine, &Malloc, &Log, &Error, &Warn, &FileManager, FConfigCacheIni::Factory, 1 );

#if __STATIC_LINK
        INT Lookup = 0;
        RegisterEngineNatives(Lookup);
        AUTO_INITIALIZE_REGISTRANTS_SDLDRV;
        AUTO_INITIALIZE_REGISTRANTS_FIRE;
        AUTO_INITIALIZE_REGISTRANTS_ALAUDIO;
        AUTO_INITIALIZE_REGISTRANTS_OPENGLDRV;
        RegisterGameNatives(Lookup);


// The linker drops these as dead code without an explicit reference.
char blah;
snprintf(&blah, 1, "%d%d%d", GLoadedSDLDrv, GLoadedOpenGLDrv, GLoadedALAudio);
#endif

		// Init mode.
		GIsServer     = 1;
		GIsClient     = !ParseParam(appCmdLine(),TEXT("SERVER"));
		GIsEditor     = 0;
		GIsScriptable = 1;
		GLazyLoad     = !GIsClient || ParseParam(appCmdLine(),TEXT("LAZY"));

		UBOOL HasContent = !GIsClient || FindContent();

		// Figure out whether to show log or splash screen.
		UBOOL ShowLog = ParseParam(CmdLine,TEXT("LOG"));

		TCHAR *LogoFName = appStaticString1024();
		if( !Parse( appCmdLine(), TEXT("-USERLOGO="), LogoFName, 1024 ) )
			appSprintf( LogoFName, TEXT("%sLogo.bmp"), GPackage );

		FString Filename = FString(TEXT("../Help")) * LogoFName;
		if ( appStrlen(GModPath)>0 )
			Filename = FString(GModPath) * FString(TEXT("Help")) * GModName + TEXT("Logo.bmp");
		if( GFileManager->FileSize(*Filename)<0 )
			Filename = FString(TEXT("../Help")) * GPackage + TEXT("Logo.bmp"); // gam
		if( HasContent && (!ShowLog) &&
            (!ParseParam(CmdLine,TEXT("server"))) &&
            (!ParseParam(CmdLine,TEXT("nosplash"))) )
			InitSplash( *Filename );

		// Benchmarking/ movie recording.
		GIsBenchmarking		= ParseParam(appCmdLine(),TEXT("BENCHMARK"));
		GIsRecordingMovie	= ParseParam(appCmdLine(),TEXT("RECORDMOVIE"));
	
		// Fixed timestep.
		FLOAT FixedFPS		= 0.f;
		Parse(appCmdLine(),TEXT("FIXEDFPS="),FixedFPS);
	
		if( GIsRecordingMovie || GIsBenchmarking || FixedFPS > 0.f )
		{
			GUseFixedTimeStep	= 1;
			GFixedTimeStep		= FixedFPS > 0.f ? 1.f / FixedFPS : 1.f / 30.f;
		}

		if( ParseParam(appCmdLine(),TEXT("NULLRENDER")) )
			GConfig->SetString( TEXT("Engine.Engine"		), TEXT("RenderDevice"		), TEXT("Engine.NullRenderDevice"	));

		// Don't update ini files if benchmarking.
		if( GIsBenchmarking || ParseParam(appCmdLine(),TEXT("NOINI")) || ParseParam(appCmdLine(),TEXT("EXITAFTERDEMO")) )
		{
			GConfig->Detach( GIni );
			GConfig->Detach( GUserIni );
		}

		// Init engine.
		UEngine* Engine = HasContent ? InitEngine() : NULL;

		if( Engine )
		{
			// Hide splash screen.
			ExitSplash();

			// Optionally Exec an exec file
			FString Temp;
			if( Parse(CmdLine, TEXT("EXEC="), Temp) )
			{
				Temp = FString(TEXT("exec ")) + Temp;
				if( Engine->Client && Engine->Client->Viewports.Num() && Engine->Client->Viewports(0) )
					Engine->Client->Viewports(0)->Exec( *Temp, *GLog );
			}

			if( !GIsClient )
				InitServerTray();

			// Start main engine loop, including the Windows message pump.
			if( !GIsRequestingExit )
				MainLoop( Engine );
			ExitServerTray();
		}

		// Clean shutdown.
		GLog->Log( NAME_Title, LocalizeGeneral(TEXT("Exit"),GPackage) );

		appPreExit();
		GIsGuarded = 0;
#ifndef _DEBUG
	}
	catch( ... )
	{
		// Crashed.
		ErrorLevel = 1;
		Error.HandleError();
		// The GUI build has no console, so the error would otherwise only be in the log.
		if( GIsClient )
		{
			// The history is one line, which the message box would size to its full width.
			FString Message = GErrorHist;
			for( INT i; (i=Message.InStr(TEXT(" <- ")))!=-1; )
				Message = Message.Left(i) + TEXT("\n") + Message.Mid(i+4);
			Message += FString::Printf( TEXT("\n\nThe log file is %s"), Log.Filename );
			SDL_ShowSimpleMessageBox( SDL_MESSAGEBOX_ERROR, "UT2004", appToAnsi(*Message), NULL );
		}
	}
#endif

#if defined(__ANDROID__) || __EMSCRIPTEN__
	// appExit would fork for it. The server starts after appExit, as it reads the inis appExit writes.
	TCHAR ServerCmd[1024] = TEXT("");
	if( GUnixSpawnOnExit && GIsClient )
		appStrncpy( ServerCmd, GUnixSpawnOnExit, ARRAY_COUNT(ServerCmd) );
	GUnixSpawnOnExit = NULL;
#endif
#if __EMSCRIPTEN__
	ANSICHAR PageServerCmd[1024];
	appToAnsi( ServerCmd, PageServerCmd, ARRAY_COUNT(PageServerCmd) );
#endif

	// Final shut down.
	appExit();
	GIsStarted = 0;
#ifdef __ANDROID__
	if( *ServerCmd )
		StartServerService( ServerCmd );
#elif __EMSCRIPTEN__
	// A crash stays on screen with its log instead.
	if( !ErrorLevel )
		ut_exit( PageServerCmd );
#endif
	return((int) ErrorLevel);
}

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/

