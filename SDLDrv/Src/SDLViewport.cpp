/*=============================================================================
	SDLViewport.cpp: USDLViewport code.
	Copyright 2002 Epic Games, Inc. All Rights Reserved.

        SDL website: http://www.libsdl.org/
=============================================================================*/

#include <stdlib.h>
#ifdef WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "GamepadMotion.hpp"
#include "SDLDrv.h"

// Duplicated here for convenience.
enum EEditorMode
{
	EM_None 				= 0,	// Gameplay, editor disabled.
	EM_ViewportMove			= 1,	// Move viewport normally.
	EM_ViewportZoom			= 2,	// Move viewport with acceleration.
	EM_ActorRotate			= 5,	// Rotate actor.
	EM_ActorScale			= 8,	// Scale actor.
	EM_TexturePan			= 11,	// Pan textures.
	EM_TextureRotate		= 13,	// Rotate textures.
	EM_TextureScale			= 14,	// Scale textures.
	EM_ActorSnapScale		= 18,	// Actor snap-scale.
	EM_TexView				= 19,	// Viewing textures.
	EM_TexBrowser			= 20,	// Browsing textures.
	EM_StaticMeshBrowser	= 21,	// Browsing static meshes.
	EM_MeshView				= 22,	// Viewing mesh.
	EM_MeshBrowser			= 23,	// Browsing mesh.
	EM_BrushClip			= 24,	// Brush Clipping.
	EM_VertexEdit			= 25,	// Multiple Vertex Editing.
	EM_FaceDrag				= 26,	// Face Dragging.
	EM_Polygon				= 27,	// Free hand polygon drawing
	EM_TerrainEdit			= 28,	// Terrain editing.
	EM_PrefabBrowser		= 29,	// Browsing prefabs.
	EM_Matinee				= 30,	// Movie editing.
	EM_EyeDropper			= 31,	// Eyedropper
	EM_Animation			= 32,	// Viewing Animation
	EM_FindActor			= 33,	// Find Actor
	EM_MaterialEditor		= 34,	// Material editor
	EM_Geometry				= 35,	// Geometry editing mode
	EM_NewCameraMove		= 50,
};

/*-----------------------------------------------------------------------------
	Gamepad aim shaping.
-----------------------------------------------------------------------------*/

// Scaled radial dead zone and response curve on the stick vector. A per axis dead zone or curve would
// pull diagonals toward the axes. Input is -1 to 1, output -0.5 to 0.5 as the ini binds expect.
// Returns the deflection past the dead zone, before the curve.
static FLOAT ShapeStick( FLOAT& X, FLOAT& Y, FLOAT DeadZone, FLOAT Exponent )
{
	FLOAT Length = appSqrt( X*X + Y*Y );
	FLOAT Deflection = Length > DeadZone ? Min( (Length - DeadZone) / (1.f - DeadZone), 1.f ) : 0.f;
	FLOAT Scale = Deflection > 0.f ? 0.5f * appPow( Deflection, Exponent ) / Length : 0.f;
	X *= Scale;
	Y *= Scale;
	return Deflection;
}

// Gyro aim after GyroWiki: GamepadMotionHelpers calibrates the bias and finds gravity for player space yaw,
// soft tiered smoothing removes hand jitter below SmoothThreshold without lagging real turns.
static GamepadMotion Motion;
static SDL_JoystickID GyroPad;
static Uint64 GyroTime;
static FLOAT GyroAccel[3];
static FLOAT GyroYaw, GyroPitch;
static FLOAT SmoothX, SmoothY;

static void SmoothGyro( FLOAT& X, FLOAT& Y, FLOAT DeltaTime )
{
	const FLOAT SmoothThreshold = 5.f, SmoothTime = 0.03f;
	FLOAT Direct = Clamp<FLOAT>( (appSqrt( X*X + Y*Y ) - 0.5f * SmoothThreshold) / (0.5f * SmoothThreshold), 0.f, 1.f );
	FLOAT Blend = 1.f - appExp( -DeltaTime / SmoothTime );
	SmoothX += (X * (1.f - Direct) - SmoothX) * Blend;
	SmoothY += (Y * (1.f - Direct) - SmoothY) * Blend;
	X = X * Direct + SmoothX;
	Y = Y * Direct + SmoothY;
}

// Collects the turn in degrees until UpdateInput sends it. Every sample counts, pads report up to 1000 per second.
// Source is the pad, or 0 for the device's own sensors.
static void ProcessGyro( SDL_JoystickID Source, SDL_SensorType Type, Uint64 SensorTime, Uint64 EventTime, const float* Data )
{
	Uint64 Time = SensorTime ? SensorTime : EventTime;
	if( Source != GyroPad )
	{
		GyroPad = Source;
		GyroTime = 0;
		GyroAccel[0] = GyroAccel[1] = GyroAccel[2] = 0.f;
		Motion.Reset();
		Motion.ResetContinuousCalibration();
		Motion.SetCalibrationMode( GamepadMotionHelpers::Stillness );
	}
	if( Type == SDL_SENSOR_ACCEL )
	{
		for( INT i=0; i<3; i++ )
			GyroAccel[i] = Data[i] / SDL_STANDARD_GRAVITY;
		return;
	}
	if( Type != SDL_SENSOR_GYRO )
		return;
	FLOAT DeltaTime = GyroTime ? (FLOAT)(Time - GyroTime) * 1e-9f : 0.f;
	// SDL's Android sensor thread stamps all samples of one wakeup alike, each still is one 60 Hz period.
	if( !Source && Time == GyroTime )
		DeltaTime = 1.f / 60.f;
	GyroTime = Time;
	// The first sample, and the first after the sensor was off, has no interval.
	if( DeltaTime <= 0.f || DeltaTime > 0.1f || (!GyroAccel[0] && !GyroAccel[1] && !GyroAccel[2]) )
		return;
	const FLOAT ToDegrees = 180.f / PI;
	Motion.ProcessMotion( Data[0] * ToDegrees, Data[1] * ToDegrees, Data[2] * ToDegrees, GyroAccel[0], GyroAccel[1], GyroAccel[2], DeltaTime );
	FLOAT Pitch, Yaw;
	Motion.GetPlayerSpaceGyro( Pitch, Yaw, 2.f );
	SmoothGyro( Yaw, Pitch, DeltaTime );
	// SDL rotations are counterclockwise, Unreal yaw turns right.
	GyroYaw -= Yaw * DeltaTime;
	GyroPitch += Pitch * DeltaTime;
}

// Device sensors report in the screen's natural orientation, pads in the landscape one they are held in.
static void ProcessDeviceGyro( const SDL_SensorEvent& Event, SDL_Window* Window )
{
	// Counterclockwise quarter turns from portrait, indexed by SDL_DisplayOrientation.
	static const INT Turns[] = { 0, 1, 3, 0, 2 };
	SDL_DisplayID Display = SDL_GetDisplayForWindow( Window );
	INT Turn = (Turns[SDL_GetCurrentDisplayOrientation( Display )] - Turns[SDL_GetNaturalDisplayOrientation( Display )]) & 3;
	float Data[3] = { Event.data[0], Event.data[1], Event.data[2] };
	for( INT i=0; i<Turn; i++ )
	{
		float X = Data[0];
		Data[0] = -Data[1];
		Data[1] = X;
	}
	ProcessGyro( 0, SDL_GetSensorTypeForID( Event.which ), Event.sensor_timestamp, Event.timestamp, Data );
}


/*-----------------------------------------------------------------------------
	Class implementation.
-----------------------------------------------------------------------------*/

IMPLEMENT_CLASS(USDLViewport);

//
// EInputKey mirrors Windows virtual keys. Printable keys follow the layout like VK codes do,
// the rest follow their physical position.
//
static EInputKey KeyFromSDL( SDL_Keycode Key, SDL_Scancode Scancode )
{
	if( Key >= SDLK_A && Key <= SDLK_Z )
		return (EInputKey)(IK_A + Key - SDLK_A);
	if( Key >= SDLK_0 && Key <= SDLK_9 )
		return (EInputKey)(IK_0 + Key - SDLK_0);
	switch( Key )
	{
		case SDLK_SPACE:		return IK_Space;
		case SDLK_GRAVE:		return IK_Tilde;
		case SDLK_APOSTROPHE:	return IK_SingleQuote;
		case SDLK_SEMICOLON:	return IK_Semicolon;
		case SDLK_COMMA:		return IK_Comma;
		case SDLK_PERIOD:		return IK_Period;
		case SDLK_SLASH:		return IK_Slash;
		case SDLK_BACKSLASH:	return IK_Backslash;
		case SDLK_LEFTBRACKET:	return IK_LeftBracket;
		case SDLK_RIGHTBRACKET:	return IK_RightBracket;
		case SDLK_MINUS:		return IK_Minus;
		case SDLK_EQUALS:		return IK_Equals;
	}
	if( Scancode >= SDL_SCANCODE_F1 && Scancode <= SDL_SCANCODE_F12 )
		return (EInputKey)(IK_F1 + Scancode - SDL_SCANCODE_F1);
	if( Scancode >= SDL_SCANCODE_F13 && Scancode <= SDL_SCANCODE_F24 )
		return (EInputKey)(IK_F13 + Scancode - SDL_SCANCODE_F13);
	if( Scancode >= SDL_SCANCODE_KP_1 && Scancode <= SDL_SCANCODE_KP_9 )
		return (EInputKey)(IK_NumPad1 + Scancode - SDL_SCANCODE_KP_1);
	switch( Scancode )
	{
		case SDL_SCANCODE_BACKSPACE:	return IK_Backspace;
		case SDL_SCANCODE_TAB:			return IK_Tab;
		case SDL_SCANCODE_RETURN:
		case SDL_SCANCODE_KP_ENTER:		return IK_Enter;
		case SDL_SCANCODE_PAUSE:		return IK_Pause;
		case SDL_SCANCODE_ESCAPE:
		case SDL_SCANCODE_AC_BACK:		return IK_Escape;
		case SDL_SCANCODE_DELETE:		return IK_Delete;
		case SDL_SCANCODE_INSERT:		return IK_Insert;
		case SDL_SCANCODE_PRINTSCREEN:	return IK_PrintScrn;
		case SDL_SCANCODE_LSHIFT:
		case SDL_SCANCODE_RSHIFT:		return IK_Shift;
		case SDL_SCANCODE_LCTRL:
		case SDL_SCANCODE_RCTRL:		return IK_Ctrl;
		case SDL_SCANCODE_LALT:
		case SDL_SCANCODE_RALT:			return IK_Alt;
		case SDL_SCANCODE_LGUI:
		case SDL_SCANCODE_RGUI:			return IK_F24;
		// The console key on layouts where it does not type a backquote.
		case SDL_SCANCODE_GRAVE:		return IK_Tilde;
		// Punctuation positions on layouts where they type other characters, like VK_OEM keys.
		case SDL_SCANCODE_APOSTROPHE:	return IK_SingleQuote;
		case SDL_SCANCODE_SEMICOLON:	return IK_Semicolon;
		case SDL_SCANCODE_COMMA:		return IK_Comma;
		case SDL_SCANCODE_PERIOD:		return IK_Period;
		case SDL_SCANCODE_SLASH:		return IK_Slash;
		case SDL_SCANCODE_BACKSLASH:	return IK_Backslash;
		case SDL_SCANCODE_LEFTBRACKET:	return IK_LeftBracket;
		case SDL_SCANCODE_RIGHTBRACKET:	return IK_RightBracket;
		case SDL_SCANCODE_MINUS:		return IK_Minus;
		case SDL_SCANCODE_EQUALS:		return IK_Equals;
		case SDL_SCANCODE_NONUSBACKSLASH:	return IK_UnknownE2;
		case SDL_SCANCODE_HOME:			return IK_Home;
		case SDL_SCANCODE_END:			return IK_End;
		case SDL_SCANCODE_PAGEUP:		return IK_PageUp;
		case SDL_SCANCODE_PAGEDOWN:		return IK_PageDown;
		case SDL_SCANCODE_LEFT:			return IK_Left;
		case SDL_SCANCODE_RIGHT:		return IK_Right;
		case SDL_SCANCODE_UP:			return IK_Up;
		case SDL_SCANCODE_DOWN:			return IK_Down;
		case SDL_SCANCODE_KP_0:			return IK_NumPad0;
		case SDL_SCANCODE_KP_MULTIPLY:	return IK_GreyStar;
		case SDL_SCANCODE_KP_PLUS:		return IK_GreyPlus;
		case SDL_SCANCODE_KP_EQUALS:	return IK_Separator;
		case SDL_SCANCODE_KP_MINUS:		return IK_GreyMinus;
		case SDL_SCANCODE_KP_PERIOD:	return IK_NumPadPeriod;
		case SDL_SCANCODE_KP_DIVIDE:	return IK_GreySlash;
		case SDL_SCANCODE_NUMLOCKCLEAR:	return IK_NumLock;
		case SDL_SCANCODE_CAPSLOCK:		return IK_CapsLock;
		case SDL_SCANCODE_SCROLLLOCK:	return IK_ScrollLock;
		default:						return IK_None;
	}
}

/*-----------------------------------------------------------------------------
	USDLViewport Init/Exit.
-----------------------------------------------------------------------------*/

//
// Constructor.
//
USDLViewport::USDLViewport()
:	UViewport()
{
	guard(USDLViewport::USDLViewport);

	FullscreenOnly = 0;
	TextToSpeechObject = -1;
	ColorBytes = 4;
	debugf( TEXT("Created and initialized a new SDL viewport.") );

	unguard;
}

//
// Whether the GUI's focused control is an editable GUIEditBox. Drivers don't link XInterface,
// so its fields are looked up by name.
//
static UBOOL EditBoxFocused( UObject* GUIController )
{
	static UClass* EditBox = FindObject<UClass>( ANY_PACKAGE, TEXT("GUIEditBox") );
	UObjectProperty* Focus = FindField<UObjectProperty>( GUIController->GetClass(), TEXT("FocusedControl") );
	UObject* Control = Focus ? *(UObject**)((BYTE*)GUIController + Focus->Offset) : NULL;
	if( !EditBox || !Control || !Control->IsA(EditBox) )
		return 0;
	UBoolProperty* ReadOnly = FindField<UBoolProperty>( EditBox, TEXT("bReadOnly") );
	return !ReadOnly || !(*(BITFIELD*)((BYTE*)Control + ReadOnly->Offset) & ReadOnly->BitMask);
}

//
// The seat count of the vehicle the player is in, 0 on foot, and in Seat the one the player holds.
// Onslaught isn't linked either. Clients fill WeaponPawns in replication order, so a passenger's seat
// comes from the bone its gun sits on, which follows the server's PassengerWeapons numbering.
//
INT USDLViewport::VehicleSeats( INT& Seat )
{
	Seat = 1;
	APawn* Pawn = Actor ? Actor->Pawn : NULL;
	if( !Pawn || !Pawn->IsA(AVehicle::StaticClass()) )
		return 0;
	UObjectProperty* BaseProp = FindField<UObjectProperty>( Pawn->GetClass(), TEXT("VehicleBase") );
	UObject* Base = BaseProp ? *(UObject**)((BYTE*)Pawn + BaseProp->Offset) : NULL;
	UObject* Vehicle = Base ? Base : Pawn;
	UArrayProperty* Seats = FindField<UArrayProperty>( Vehicle->GetClass(), TEXT("PassengerWeapons") );
	UStructProperty* Inner = Seats ? Cast<UStructProperty>( Seats->Inner ) : NULL;
	if( !Inner )
		return 1;
	FArray* Array = (FArray*)((BYTE*)Vehicle + Seats->Offset);
	UObjectProperty* GunProp = Base ? FindField<UObjectProperty>( Pawn->GetClass(), TEXT("Gun") ) : NULL;
	AActor* Gun = GunProp ? Cast<AActor>( *(UObject**)((BYTE*)Pawn + GunProp->Offset) ) : NULL;
	UNameProperty* Bone = FindField<UNameProperty>( Inner->Struct, TEXT("WeaponBone") );
	if( Gun && Bone )
		for( INT i=0; i<Array->Num(); i++ )
			if( *(FName*)((BYTE*)Array->GetData() + i * Inner->ElementSize + Bone->Offset) == Gun->AttachmentBone )
				Seat = i + 2;
	return Array->Num() + 1;
}

//
// Phone and tablet screens are dense, the fonts grow towards their size on a 2000 point wide screen.
//
FLOAT USDLViewport::GUIFontScale()
{
	SDL_Window* Window = GetOuterUSDLClient()->Window;
	if( !Window || !SDL_HasScreenKeyboardSupport() )
		return 1.f;
	FLOAT Points = SizeX / Max( SDL_GetDisplayContentScale( SDL_GetDisplayForWindow( Window ) ), 1.f );
	return Clamp( 2000.f / Points, 1.f, 2.5f );
}

//
// Destroy.
//
void USDLViewport::Destroy()
{
	guard(USDLViewport::Destroy);
	Super::Destroy();

	if( BlitFlags & BLIT_Temporary )
		appFree( ScreenPointer );

    if (TextToSpeechObject != -1)
    {
        close(TextToSpeechObject);
        TextToSpeechObject = -1;
    }

	unguard;
}

//
// Error shutdown.
//
void USDLViewport::ShutdownAfterError()
{
	SDL_Quit();
	Super::ShutdownAfterError();
}


//
// Fullscreen and grabbed windows read raw relative motion, unless a windowed menu shows the OS cursor
// or the cursor works the on-screen controls.
//
void USDLViewport::UpdateMouseGrabState()
{
	SDL_Window* Window = GetOuterUSDLClient()->Window;
	SDL_SetWindowRelativeMouseMode( Window, (IsFullscreen() || WantGrab) && !WindowsMouseShown && !VJShown );
	MouseIsGrabbed = SDL_GetWindowRelativeMouseMode( Window );
}


/*-----------------------------------------------------------------------------
	Command line.
-----------------------------------------------------------------------------*/

//
// Command line.
//
UBOOL USDLViewport::Exec( const TCHAR* Cmd, FOutputDevice& Ar )
{
	guard(USDLViewport::Exec);
	if( UViewport::Exec( Cmd, Ar ) )
	{
		return 1;
	}
	else if( ParseCommand(&Cmd,TEXT("ENDFULLSCREEN")) )
	{
		if( BlitFlags & BLIT_Fullscreen )
			EndFullscreen();
		return 1;
	}
	else if( ParseCommand(&Cmd,TEXT("TOGGLEFULLSCREEN")) )
	{
		ToggleFullscreen();
		return 1;
	}
	else if( ParseCommand(&Cmd,TEXT("NEXTSEAT")) )
	{
		// Like the number keys in a vehicle. A taken seat refuses, so the next press moves on past it.
		INT Seat, Seats = VehicleSeats( Seat );
		if( Seats > 1 )
		{
			if( Actor->Pawn != LastSeatPawn )
			{
				LastSeatPawn = Actor->Pawn;
				LastSeat = Seat;
			}
			LastSeat = LastSeat % Seats + 1;
			if( LastSeat == Seat )
				LastSeat = LastSeat % Seats + 1;
			Exec( *FString::Printf( TEXT("SwitchWeapon %d"), LastSeat ), Ar );
		}
		return 1;
	}
	else if( ParseCommand(&Cmd,TEXT("GETCURRENTRES")) )
	{
		Ar.Logf( TEXT("%ix%i"), SizeX, SizeY );
		return 1;
	}
	else if( ParseCommand(&Cmd,TEXT("GETCURRENTRENDERDEVICE")) )
	{
		Ar.Log( RenDev->GetClass()->GetPathName() );
		return 1;
	}
	else if( ParseCommand(&Cmd,TEXT("SETRES")) )
	{
		SetResolution( Cmd, 1 );
		return 1;
	}
	else if( ParseCommand(&Cmd,TEXT("TEMPSETRES")) )
	{
		SetResolution( Cmd, 0 );
		return 1;
	}
	else if( ParseCommand(&Cmd,TEXT("VIRTUALJOYSTICK")) )
	{
		USDLClient* Client = GetOuterUSDLClient();
		Client->VJWanted = !Client->VJWanted;
		SwitchVirtualJoystick( Client->VJWanted );
		Ar.Logf( TEXT("Virtual joystick %s"), USDLClient::VirtualPad ? TEXT("on") : TEXT("off") );
		return 1;
	}

	else if( ParseCommand(&Cmd,TEXT("SETMOUSE")) )
	{
	    const TCHAR* CmdTemp = Cmd;
		INT X, Y;
		
		X = appAtoi(CmdTemp);
		
		while( appIsDigit( *CmdTemp) )
		    CmdTemp++;

		while( appIsSpace( *CmdTemp) )
		    CmdTemp++;
		    
		Y = appAtoi(CmdTemp);
		
        SDL_WarpMouseInWindow( GetOuterUSDLClient()->Window, X, Y );
        return 1;
	}

	else if( ParseCommand(&Cmd,TEXT("TTS")) )
	{
		if( appStrcmp(Cmd,TEXT("")) != 0 )
			TextToSpeech( FString(Cmd), 1.f );
		return 1;
	}

        // vi style quit commands.
    else if ((ParseCommand(&Cmd,TEXT(":wq"))) || (ParseCommand(&Cmd,TEXT(":q!"))))
    {
        GIsRequestingExit = 1;
        return 1;
    }

	else return 0;
	unguard;
}

/*-----------------------------------------------------------------------------
	Window openining and closing.
-----------------------------------------------------------------------------*/

//
// Open this viewport's window.
//
void USDLViewport::OpenWindow( PTRINT InParentWindow, UBOOL IsTemporary, INT NewX, INT NewY, INT OpenX, INT OpenY )
{
	guard(USDLViewport::OpenWindow);

	check(Actor);
	UBOOL DoRepaint=0, DoSetActive=0;
	USDLClient* C = GetOuterUSDLClient();

    if (TextToSpeechObject == -1)
    {
        if (C->TextToSpeechFile.Len() == 0)
            debugf(TEXT("TTS: No output filename specified."));
        else
        {
            // Please note we don't create this file...
            TextToSpeechObject = open(appToAnsi(*(C->TextToSpeechFile)), O_WRONLY);
            if (TextToSpeechObject != -1)
                debugf(TEXT("TTS: Opened file \"%s\" for text-to-speech output."), *(C->TextToSpeechFile));
            else
            {
                int err = errno;
                debugf(TEXT("TTS: Couldn't open TTS file \"%s\""), *(C->TextToSpeechFile));
                debugf(TEXT("TTS: System error is \"%s\" (%d)."), appFromAnsi(strerror(err)), err);
                if (err == ENOENT)
                {
                    debugf(TEXT("TTS: (We intentionally don't create this file if it doesn't exist!)"));
                    debugf(TEXT("TTS: Disabling Text-to-speech support..."));
                }
            }
        }
    }

	if( NewX!=INDEX_NONE )
		NewX = Align( NewX, 2 );

	// Create frame buffer.
	if( IsTemporary )
	{
		// Create in-memory data.
		BlitFlags     = BLIT_Temporary;
		ColorBytes    = 2;
		SizeX         = NewX;
		SizeY         = NewY;
		ScreenPointer = (BYTE*)appMalloc( 2 * NewX * NewY, TEXT("TemporaryViewportData") );	
		debugf( NAME_Log, TEXT("Opened temporary viewport") );
   	}

	// Create rendering device.
	if( !RenDev && !appStrcmp( GetName(), TEXT("VisibiltyViewport") ) )
		TryRenderDevice( TEXT("Editor.VisibilityRenderDevice"), NewX, NewY, 0 );
	if( !RenDev && (BlitFlags & BLIT_Temporary) )
		TryRenderDevice( TEXT("NullDrv.NullRenderDevice"), NewX, NewY, 0 );
	if( !RenDev && !GIsEditor && ParseParam(appCmdLine(),TEXT("windowed")) )
		TryRenderDevice( TEXT("ini:Engine.Engine.RenderDevice"), NewX, NewY, 0 );
	if( !RenDev )
		TryRenderDevice( TEXT("ini:Engine.Engine.RenderDevice"), NewX, NewY, GIsEditor ? 0 : C->StartupFullscreen );

	check(RenDev);
	UpdateWindowFrame();

	unguard;
}

//
// Close a viewport window.  Assumes that the viewport has been opened with
// OpenViewportWindow.  Does not affect the viewport's object, only the
// platform-specific information associated with it.
//
void USDLViewport::CloseWindow()
{
	guard(USDLViewport::CloseWindow);
    /* no-op. */
	unguard;
}

/*-----------------------------------------------------------------------------
	USDLViewport operations.
-----------------------------------------------------------------------------*/

//
// Set window position according to menu's on-top setting:
//
void USDLViewport::SetTopness()
{
	guard(USDLViewport::SetTopness);
    /* no-op. */
	unguard;
}

//
// Repaint the viewport.
//
void USDLViewport::Repaint( UBOOL Blit )
{
	guard(USDLViewport::Repaint);
	GetOuterUSDLClient()->Engine->Draw( this, Blit );
	unguard;
}

//
// Return whether fullscreen.
//
UBOOL USDLViewport::IsFullscreen()
{
	guard(USDLViewport::IsFullscreen);
	return (BlitFlags & BLIT_Fullscreen)!=0;
	unguard;
}

void USDLViewport::SetModeCursor()
{
}


void USDLViewport::SetTitleBar()
{
    TCHAR WindowName[80];
	appSnprintf( WindowName, ARRAY_COUNT(WindowName), TEXT("%s"), LocalizeGeneral("Product",appPackage()) );

	SDL_SetWindowTitle( GetOuterUSDLClient()->Window, appToAnsi(WindowName) );
}


//
// Update user viewport interface.
//
void USDLViewport::UpdateWindowFrame()
{
	guard(USDLViewport::UpdateWindowFrame);

	// If not a window, exit.
	if( !GetOuterUSDLClient()->Window || (BlitFlags&BLIT_Fullscreen) || (BlitFlags&BLIT_Temporary) || !Actor)
		return;

    SetTitleBar();

	unguard;
}

//
// The GUI reads the window cursor position when it shows the OS mouse.
//
void USDLViewport::UpdateMousePosition()
{
	float X, Y;
	SDL_GetMouseState( &X, &Y );
	WindowsMouseX = X;
	WindowsMouseY = Y;
}

//
// Pixel layout of the on-screen controls: the move stick bottom left, the look stick bottom right
// and the buttons in an arc over the look stick, where the right thumb reaches.
// Use sits above and inside the move stick for the left thumb.
//
void USDLViewport::VJLayout( INT i, FLOAT& X, FLOAT& Y, FLOAT& R )
{
	FLOAT StickR = SizeY * 0.11f, Margin = SizeY * 0.06f;
	X = i == VJ_Move || i == VJ_Use ? Margin + StickR : SizeX - Margin - StickR;
	Y = SizeY - Margin - StickR;
	R = StickR;
	if( i >= VJ_Fire )
	{
		static const FLOAT Angles[] = { 170.f, 135.f, 100.f, 65.f, 60.f };
		FLOAT A = Angles[i - VJ_Fire] * PI / 180.f;
		X += appCos( A ) * StickR * 1.7f;
		Y -= appSin( A ) * StickR * 1.7f;
		R = StickR * 0.42f;
	}
}

//
// Writes one control to the virtual pad. SDL would write into a physical pad's driver data.
//
void USDLViewport::VJWrite( INT i, UBOOL Held, FLOAT X, FLOAT Y )
{
	guard(USDLViewport::VJWrite);
	if( !USDLClient::VirtualPad )
		return;
	SDL_Joystick* Joystick = SDL_GetGamepadJoystick( USDLClient::Gamepad );
	if( i == VJ_Move || i == VJ_Look )
	{
		FLOAT CX, CY, R;
		VJLayout( i, CX, CY, R );
		FLOAT DX = Held ? (X - CX) / R : 0.f, DY = Held ? (Y - CY) / R : 0.f;
		FLOAT Len = appSqrt( DX * DX + DY * DY );
		if( Len > 1.f )
		{
			DX /= Len;
			DY /= Len;
		}
		INT Axis = i == VJ_Move ? SDL_GAMEPAD_AXIS_LEFTX : SDL_GAMEPAD_AXIS_RIGHTX;
		SDL_SetJoystickVirtualAxis( Joystick, Axis, (Sint16)(DX * 32767.f) );
		SDL_SetJoystickVirtualAxis( Joystick, Axis + 1, (Sint16)(DY * 32767.f) );
	}
	else if( i == VJ_Jump )
		SDL_SetJoystickVirtualButton( Joystick, 0, Held != 0 );
	// The second button is the pad's east button, bound to Duck.
	else if( i == VJ_Crouch )
		SDL_SetJoystickVirtualButton( Joystick, 1, Held != 0 );
	// The third button is the pad's west button, bound to Use like E.
	else if( i == VJ_Use )
		SDL_SetJoystickVirtualButton( Joystick, 2, Held != 0 );
	else
		SDL_SetJoystickVirtualAxis( Joystick, i == VJ_Fire ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER : SDL_GAMEPAD_AXIS_LEFT_TRIGGER, Held ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN );
	unguard;
}

//
// A pointer going down, moving or going up, as finger event types. Returns whether a control took it.
// Pen and touch can share finger ids, so the touch id is matched too.
//
UBOOL USDLViewport::VJPointer( SDL_TouchID Touch, SDL_FingerID Finger, Uint32 Phase, FLOAT X, FLOAT Y )
{
	guard(USDLViewport::VJPointer);
	INT i = 0;
	while( i < VJ_Count && !(VJFinger[i] == Finger && VJTouch[i] == Touch) )
		i++;
	if( Phase == SDL_EVENT_FINGER_DOWN && i == VJ_Count )
	{
		// Buttons first, the generous stick grab area reaches over them.
		for( i=VJ_Count-1; i>=0; i-- )
		{
			FLOAT CX, CY, R;
			VJLayout( i, CX, CY, R );
			FLOAT Grab = i <= VJ_Look ? R * 1.5f : R * 1.2f;
			if( !VJFinger[i] && Square(X - CX) + Square(Y - CY) < Grab * Grab )
				break;
		}
		if( i < 0 )
		{
			for( INT t=0; t<TouchTargets.Num(); t++ )
			{
				const FTouchTarget& T = TouchTargets(t);
				if( X >= T.X1 && X < T.X2 && Y >= T.Y1 && Y < T.Y2 )
				{
					Exec( *T.Command, *GLog );
					return 1;
				}
			}
			return 0;
		}
		VJTouch[i] = Touch;
		VJFinger[i] = Finger;
	}
	else if( i == VJ_Count )
		return 0;
	UBOOL Held = Phase == SDL_EVENT_FINGER_DOWN || Phase == SDL_EVENT_FINGER_MOTION;
	VJWrite( i, Held, X, Y );
	if( !Held )
		VJFinger[i] = 0;
	return 1;
	unguard;
}

//
// Turns the on-screen controls on or off, which swaps the virtual pad for a real one.
//
void USDLViewport::SwitchVirtualJoystick( UBOOL On )
{
	guard(USDLViewport::SwitchVirtualJoystick);
	GetOuterUSDLClient()->SetVirtualJoystick( On );
	// The pad block stops polling once no pad is open, so held pad keys would never release.
	PadHeld = 0;
	if( GUIController )
		GUIController->ResetInput();
	Input->ResetInput();
	unguard;
}

void USDLViewport::AddTouchTarget( FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2, const TCHAR* Command )
{
	FTouchTarget* T = new(TouchTargets) FTouchTarget;
	T->X1 = X1;
	T->Y1 = Y1;
	T->X2 = X2;
	T->Y2 = Y2;
	T->Command = Command;
}

void USDLViewport::VJRelease()
{
	guard(USDLViewport::VJRelease);
	for( INT i=0; i<VJ_Count; i++ )
		if( VJFinger[i] )
		{
			VJWrite( i, 0, 0.f, 0.f );
			VJFinger[i] = 0;
		}
	unguard;
}

static void DrawDisc( FCanvasUtil* CU, UMaterial* Material, FLOAT X, FLOAT Y, FLOAT R, FColor Color )
{
	const INT Segments = 40;
	Color = Color.RenderColor();
	CU->BeginPrimitive( PT_TriangleList, Material );
	for( INT i=0; i<Segments; i++ )
	{
		FLOAT A0 = 2.f * PI * i / Segments, A1 = 2.f * PI * (i + 1) / Segments;
		new(CU->Vertices) FCanvasVertex( FVector(X, Y, 0), Color, 0.f, 0.f );
		new(CU->Vertices) FCanvasVertex( FVector(X + appCos(A0) * R, Y + appSin(A0) * R, 0), Color, 0.f, 0.f );
		new(CU->Vertices) FCanvasVertex( FVector(X + appCos(A1) * R, Y + appSin(A1) * R, 0), Color, 0.f, 0.f );
	}
	CU->NumPrimitives += Segments;
}

//
// Draws the on-screen controls. Alpha blended 2D drawn before the view model stays out of the world's post processing.
//
void USDLViewport::RenderOverlay()
{
	guard(USDLViewport::RenderOverlay);
	if( !VJShown || !Canvas->pCanvasUtil )
		return;

	static FSolidColorTexture WhiteTexture( FColor(255,255,255,255) );
	DECLARE_STATIC_UOBJECT( UProxyBitmapMaterial, WhiteMaterial, { WhiteMaterial->SetTextureInterface(&WhiteTexture); } );
	DECLARE_STATIC_UOBJECT( UFinalBlend, WhiteBlend, { WhiteBlend->FrameBufferBlending = FB_AlphaBlend; WhiteBlend->Material = WhiteMaterial; WhiteBlend->ZWrite = 0; WhiteBlend->ZTest = 0; } );

	static const FColor Tints[] = { FColor(230,70,60,0), FColor(240,160,40,0), FColor(80,200,90,0), FColor(60,190,190,0), FColor(70,140,230,0) };
	static const TCHAR* Captions[] = { TEXT("FIRE"), TEXT("ALT"), TEXT("JUMP"), TEXT("CROUCH"), TEXT("USE") };
	FCanvasUtil* CU = Canvas->pCanvasUtil;
	for( INT i=0; i<VJ_Count; i++ )
	{
		FLOAT X, Y, R;
		VJLayout( i, X, Y, R );
		UBOOL Held = VJFinger[i] != 0;
		if( i == VJ_Move || i == VJ_Look )
		{
			SDL_GamepadAxis Axis = i == VJ_Move ? SDL_GAMEPAD_AXIS_LEFTX : SDL_GAMEPAD_AXIS_RIGHTX;
			FLOAT KX = SDL_GetGamepadAxis( USDLClient::Gamepad, Axis ) / 32767.f, KY = SDL_GetGamepadAxis( USDLClient::Gamepad, (SDL_GamepadAxis)(Axis + 1) ) / 32767.f;
			DrawDisc( CU, WhiteBlend, X, Y, R, FColor(255,255,255,40) );
			DrawDisc( CU, WhiteBlend, X, Y, R * 0.94f, FColor(0,0,0,70) );
			DrawDisc( CU, WhiteBlend, X + KX * R, Y + KY * R, R * 0.45f, FColor(255,255,255,Held ? 170 : 100) );
		}
		else
		{
			FColor Tint = Tints[i - VJ_Fire];
			Tint.A = Held ? 190 : 110;
			DrawDisc( CU, WhiteBlend, X, Y, R, Tint );
		}
	}

	Canvas->Style = STY_Alpha;
	Canvas->Color = FColor(255,255,255,220);
	for( INT i=VJ_Fire; i<VJ_Count; i++ )
	{
		FLOAT X, Y, R, XL, YL;
		VJLayout( i, X, Y, R );
		const TCHAR* Caption = Captions[i - VJ_Fire];
		Canvas->ClippedStrLen( Canvas->SmallFont, 1.f, 1.f, XL, YL, Caption );
		Canvas->CurX = X - XL * 0.5f;
		Canvas->CurY = Y - YL * 0.5f;
		Canvas->ClippedPrint( Canvas->SmallFont, 1.f, 1.f, 0, Caption );
	}

	// Left thumb reach above Use, in vehicles with more than one seat.
	INT Seat;
	if( VehicleSeats( Seat ) > 1 )
	{
		FLOAT X, Y, R, XL, YL;
		VJLayout( VJ_Use, X, Y, R );
		Y -= R * 2.6f;
		DrawDisc( CU, WhiteBlend, X, Y, R, FColor(170,90,220,110) );
		Canvas->ClippedStrLen( Canvas->SmallFont, 1.f, 1.f, XL, YL, TEXT("SEAT") );
		Canvas->CurX = X - XL * 0.5f;
		Canvas->CurY = Y - YL * 0.5f;
		Canvas->ClippedPrint( Canvas->SmallFont, 1.f, 1.f, 0, TEXT("SEAT") );
		AddTouchTarget( X - R, Y - R, X + R, Y + R, TEXT("NEXTSEAT") );
	}
	CU->Flush();
	// The HUD expects the canvas as Update left it.
	Canvas->eventReset();
	unguard;
}

//
// Return the viewport's window.
//
void* USDLViewport::GetWindow()
{
	guard(USDLViewport::GetWindow);
	SDL_Window* Window = GetOuterUSDLClient()->Window;
#ifdef WIN32
	// Force feedback needs the native window.
	return SDL_GetPointerProperty( SDL_GetWindowProperties(Window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL );
#else
	return Window;
#endif
	unguard;
}

/*-----------------------------------------------------------------------------
	Input.
-----------------------------------------------------------------------------*/

//
// Input event router.
//
UBOOL USDLViewport::CauseInputEvent( INT iKey, EInputAction Action, FLOAT Delta )
{
	guard(USDLViewport::CauseInputEvent);
	return GetOuterUSDLClient()->Engine->InputEvent( this, (EInputKey)iKey, Action, Delta );
	unguard;
}

//
// If the cursor is currently being captured, stop capturing, clipping, and 
// hiding it, and move its position back to where it was when it was initially
// captured.
//
void USDLViewport::SetMouseCapture( UBOOL Capture, UBOOL Clip, UBOOL OnlyFocus )
{
	guard(USDLViewport::SetMouseCapture);
	WantGrab = Capture;
	UpdateMouseGrabState();
	unguard;
}


//
// Update input for this viewport.
//
enum { TAP_None, TAP_Wait, TAP_WaitRelease, TAP_Press, TAP_PressRelease, TAP_Release };

void USDLViewport::UpdateTap()
{
	USDLClient* Client = GetOuterUSDLClient();
	// A tap moves the pointer and presses in the same frame, but the GUI picks the control under the
	// pointer in the tick after it renders the move. So the press waits two frames and the release one more.
	switch( TapState )
	{
		case TAP_Wait:
			TapState = TAP_Press;
			break;
		case TAP_WaitRelease:
			TapState = TAP_PressRelease;
			break;
		case TAP_Press:
		case TAP_PressRelease:
			// The player may have closed the keyboard while an edit box kept its focus.
			if( TextWanted && !SDL_TextInputActive( Client->Window ) )
				SDL_StartTextInput( Client->Window );
			CauseInputEvent( IK_LeftMouse, IST_Press );
			TapState = TapState==TAP_PressRelease ? TAP_Release : TAP_None;
			break;
		case TAP_Release:
			CauseInputEvent( IK_LeftMouse, IST_Release );
			TapState = TAP_None;
			break;
	}
}

void USDLViewport::UpdateGamepad( UBOOL InMenu, FLOAT DeltaSeconds )
{
	USDLClient* Client = GetOuterUSDLClient();
    if( USDLClient::Gamepad )
    {
        // Guide and Misc1 belong to the system overlay and capture, so the triggers take their keys.
        // Menus get the same keys, the GUI maps them to its own navigation.
        // Axes span -0.5 to 0.5, the range the ini DeadZone values assume.
        // The last four are the left stick pushed up, down, left and right, which works as the d-pad in menus.
        static const EInputKey Keys[SDL_GAMEPAD_BUTTON_MISC1 + 7] = { IK_Joy1, IK_Joy2, IK_Joy3, IK_Joy4, IK_Joy5, IK_None, IK_Joy7, IK_Joy8, IK_Joy9, IK_Joy10, IK_Joy11, IK_Joy13, IK_Joy14, IK_Joy15, IK_Joy16, IK_None, IK_Joy6, IK_Joy12, IK_Joy13, IK_Joy14, IK_Joy15, IK_Joy16 };
        static const EInputKey Axes[SDL_GAMEPAD_AXIS_COUNT] = { IK_JoyX, IK_JoyY, IK_JoyV, IK_JoySlider1, IK_JoyZ, IK_JoyR };
        SDL_Gamepad* Pad = USDLClient::Gamepad;
        SDL_UpdateGamepads();
        FLOAT StickX = SDL_GetGamepadAxis( Pad, SDL_GAMEPAD_AXIS_LEFTX ) / 32767.f, StickY = SDL_GetGamepadAxis( Pad, SDL_GAMEPAD_AXIS_LEFTY ) / 32767.f;
        for( INT i=0; i<ARRAY_COUNT(Keys); i++ )
        {
            DWORD Bit = 1 << i;
            UBOOL Pressed;
            if( i <= SDL_GAMEPAD_BUTTON_MISC1 )
                Pressed = SDL_GetGamepadButton( Pad, (SDL_GamepadButton)i );
            else if( i <= SDL_GAMEPAD_BUTTON_MISC1 + 2 )
            {
                // Hysteresis, so a trigger resting near the threshold does not chatter.
                SDL_GamepadAxis Trigger = i == SDL_GAMEPAD_BUTTON_MISC1 + 1 ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
                Pressed = SDL_GetGamepadAxis( Pad, Trigger ) > ((PadHeld & Bit) ? 6000 : 10000);
            }
            else
            {
                // Along the dominant axis only, so a diagonal steps once.
                INT Dir = i - (SDL_GAMEPAD_BUTTON_MISC1 + 3);
                FLOAT V = Dir < 2 ? StickY : StickX, Other = Dir < 2 ? StickX : StickY;
                if( Dir == 0 || Dir == 2 )
                    V = -V;
                Pressed = (PadHeld & Bit) ? V > 0.35f : V > 0.5f && V > Abs( Other );
            }
            // Keys follow press edges only, so a button held while a menu opens or closes does not fire late.
            // The stick is tracked in game too but only sends keys in menus, so one already pushed when a menu opens does not step it.
            UBOOL Send = Keys[i] && (InMenu || i <= SDL_GAMEPAD_BUTTON_MISC1 + 2);
            if( Pressed && !(PadHeld & Bit) )
            {
                PadHeld |= Bit;
                // Y's best weapon has nothing to pick in a vehicle, there it changes seats.
                INT Seat;
                if( Send && Keys[i] == IK_Joy4 && !InMenu && VehicleSeats( Seat ) > 1 )
                    Exec( TEXT("NEXTSEAT"), *GLog );
                else if( Send )
                    CauseInputEvent( Keys[i], IST_Press );
            }
            else if( !Pressed && (PadHeld & Bit) )
            {
                PadHeld &= ~Bit;
                if( Send )
                    CauseInputEvent( Keys[i], IST_Release );
            }
        }
        if( !InMenu )
        {
            FLOAT V[SDL_GAMEPAD_AXIS_COUNT];
            for( INT i=0; i<ARRAY_COUNT(V); i++ )
                V[i] = SDL_GetGamepadAxis( Pad, (SDL_GamepadAxis)i ) / 32767.f;
            FLOAT DeadZone = Clamp( Client->PadDeadZone, 0.f, 0.9f );
            ShapeStick( V[SDL_GAMEPAD_AXIS_LEFTX], V[SDL_GAMEPAD_AXIS_LEFTY], DeadZone, 1.f );
            FLOAT Look = ShapeStick( V[SDL_GAMEPAD_AXIS_RIGHTX], V[SDL_GAMEPAD_AXIS_RIGHTY], DeadZone, Max( Client->PadLookExponent, 0.1f ) );
            // Holding the look stick at the edge ramps up the turn rate, so fine aim and quick turns both work.
            PadEdgeTime = Look > 0.95f ? PadEdgeTime + DeltaSeconds : 0.f;
            V[SDL_GAMEPAD_AXIS_RIGHTX] *= 1.f + Client->PadTurnBoost * Clamp( (PadEdgeTime - 0.15f) / 0.3f, 0.f, 1.f );
            V[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] *= 0.5f;
            V[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] *= 0.5f;
            for( INT i=0; i<ARRAY_COUNT(Axes); i++ )
                Input->DirectAxis( Axes[i], V[i], DeltaSeconds );
        }
    }
}

void USDLViewport::UpdateCursor( UBOOL InMenu )
{
	// Windowed menus use the OS cursor, so the menu cursor is where the mouse is.
	// UGameEngine::Tick releases and restores the grab for them.
	// The on-screen controls show in game only, opening a menu lets go of them.
	UBOOL ShowVJ = USDLClient::VirtualPad && !InMenu;
#if __IOS__
	// The iOS touch overlay takes the place of these controls (SDLTouchIOS.cpp).
	ShowVJ = 0;
#endif
	if( bShowWindowsMouse != WindowsMouseShown || ShowVJ != VJShown )
	{
		WindowsMouseShown = bShowWindowsMouse;
		VJRelease();
		VJShown = ShowVJ;
		UpdateMouseGrabState();
	}
	bool WantCursor = (WindowsMouseShown && !bHideCursor) || VJShown;
	if( WantCursor != SDL_CursorVisible() )
	{
		if( WantCursor )
			SDL_ShowCursor();
		else
			SDL_HideCursor();
	}
}

void USDLViewport::UpdateTextInput( UBOOL InMenu )
{
	USDLClient* Client = GetOuterUSDLClient();
	// Text input only while a menu or the console reads it, so an IME does not take game keys.
	// An on-screen keyboard would cover the menus, so there it waits for a focused edit box.
	// ExtendedConsole's full console sets only its state, bVisible also flashes for demo speed changes.
	static FName NAME_ConsoleVisible( TEXT("ConsoleVisible") );
	FStateFrame* ConsoleState = Console ? Console->GetStateFrame() : NULL;
	UBOOL ConsoleText = Console && Console->IsA(UConsole::StaticClass())
		&& (((UConsole*)Console)->bTyping || (ConsoleState && ConsoleState->StateNode && ConsoleState->StateNode->GetFName() == NAME_ConsoleVisible));
	UBOOL WantText = SDL_HasScreenKeyboardSupport()
		? ConsoleText || (InMenu && EditBoxFocused( GUIController ))
		: ConsoleText || InMenu;
	// Only on a change, so a keyboard the player closed stays closed.
	if( WantText != TextWanted )
	{
		TextWanted = WantText;
		if( WantText )
			SDL_StartTextInput( Client->Window );
		else
			SDL_StopTextInput( Client->Window );
	}
}

void USDLViewport::KeyEvent( const SDL_KeyboardEvent& Event )
{
	USDLClient* Client = GetOuterUSDLClient();
	UBOOL Down = Event.type == SDL_EVENT_KEY_DOWN;
	if( Down && Event.key == SDLK_G && (Event.mod & SDL_KMOD_CTRL) )
	{
		if( !Event.repeat )
		{
			WantGrab = !WantGrab;
			UpdateMouseGrabState();
		}
		return;
	}
	if( Down && Event.key == SDLK_RETURN && (Event.mod & SDL_KMOD_ALT) )
	{
		if( !Event.repeat )
			ToggleFullscreen();
		return;
	}

	// Repeats press again, like WM_KEYDOWN.
	EInputKey Key = KeyFromSDL( Event.key, Event.scancode );
	if( Key != IK_None )
		CauseInputEvent( Key, Down ? IST_Press : IST_Release );

	// Text input leaves out the control characters WM_CHAR sends, edit boxes use them for editing and the clipboard.
	// Ctrl+Alt is AltGr, which types text.
	if( Down && (Key == IK_Backspace || Key == IK_Tab || Key == IK_Escape) )
		Client->Engine->Key( this, Key, Key );
	else if( Down && Key >= IK_A && Key <= IK_Z && (Event.mod & SDL_KMOD_CTRL) && !(Event.mod & SDL_KMOD_ALT) )
		Client->Engine->Key( this, (EInputKey)(Key - IK_A + 1), Key - IK_A + 1 );
}

void USDLViewport::MouseButtonEvent( const SDL_MouseButtonEvent& Event, UBOOL InMenu, FLOAT& DX, FLOAT& DY )
{
	static const EInputKey MouseButtons[] = { IK_None, IK_LeftMouse, IK_MiddleMouse, IK_RightMouse, IK_Mouse4, IK_Mouse5, IK_Mouse6, IK_Mouse7, IK_Mouse8 };
	USDLClient* Client = GetOuterUSDLClient();
	// Touch and pen arrive as fingers too. VJShown follows a frame later, VirtualPad at once.
	if( VJShown && USDLClient::VirtualPad && Event.which != SDL_TOUCH_MOUSEID && Event.which != SDL_PEN_MOUSEID )
		SwitchVirtualJoystick( 0 );
	if( Event.which == SDL_TOUCH_MOUSEID && Event.button == SDL_BUTTON_LEFT )
	{
		if( InMenu && Event.type == SDL_EVENT_MOUSE_BUTTON_DOWN )
		{
			FLOAT Density = SDL_GetWindowPixelDensity( Client->Window );
			WindowsMouseX = Event.x * Density;
			WindowsMouseY = Event.y * Density;
			// The motion before it is already in this position.
			DX = DY = 0;
			if( TapState == TAP_Release )
				CauseInputEvent( IK_LeftMouse, IST_Release );
			TapState = TAP_Wait;
			return;
		}
		// Also when the menu closed meanwhile, so the press still comes before its release.
		if( Event.type == SDL_EVENT_MOUSE_BUTTON_UP && (TapState == TAP_Wait || TapState == TAP_Press) )
		{
			TapState = TapState == TAP_Wait ? TAP_WaitRelease : TAP_PressRelease;
			return;
		}
	}
	// Touch and pen drive the on-screen controls as fingers. VJWanted, as SDL sends this before the finger that brings them back.
	if( !InMenu && Client->VJWanted && (Event.which == SDL_TOUCH_MOUSEID || Event.which == SDL_PEN_MOUSEID) )
		return;
	if( Event.button < ARRAY_COUNT(MouseButtons) && MouseButtons[Event.button] != IK_None )
		CauseInputEvent( MouseButtons[Event.button], Event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? IST_Press : IST_Release );
}

void USDLViewport::MouseWheelEvent( const SDL_MouseWheelEvent& Event )
{
	// Precise wheels and touchpads send fractions of a notch.
	FLOAT Wheel = Event.direction == SDL_MOUSEWHEEL_FLIPPED ? -Event.y : Event.y;
	if( Wheel * WheelRemainder < 0.f )
		WheelRemainder = 0.f;
	WheelRemainder += Wheel;
	while( Abs(WheelRemainder) >= 1.f )
	{
		EInputKey Key = WheelRemainder > 0.f ? IK_MouseWheelUp : IK_MouseWheelDown;
		CauseInputEvent( Key, IST_Press );
		CauseInputEvent( Key, IST_Release );
		WheelRemainder -= WheelRemainder > 0.f ? 1.f : -1.f;
	}
}

void USDLViewport::MouseMotionEvent( const SDL_MouseMotionEvent& Event, UBOOL InMenu, UBOOL& MouseMoved, FLOAT& DX, FLOAT& DY )
{
	USDLClient* Client = GetOuterUSDLClient();
	// The cursor is absolute while the on-screen controls show, so it does not turn the view.
	if( VJShown )
	{
		if( USDLClient::VirtualPad && Event.which != SDL_TOUCH_MOUSEID && Event.which != SDL_PEN_MOUSEID )
			SwitchVirtualJoystick( 0 );
		return;
	}
	if( !InMenu && Client->VJWanted && (Event.which == SDL_TOUCH_MOUSEID || Event.which == SDL_PEN_MOUSEID) )
		return;
	if ((!MouseIsGrabbed) && (Client->IgnoreUngrabbedMouse))
		return;

	MouseMoved = true;
	DX += Event.xrel;
	DY += Event.yrel;
}

void USDLViewport::GamepadEvent( const SDL_Event& Event )
{
	USDLClient* Client = GetOuterUSDLClient();
	switch( Event.type )
	{
		case SDL_EVENT_GAMEPAD_ADDED:
			if( Client->UseJoystick && !USDLClient::Gamepad )
				Client->OpenGamepad();
			else if( !USDLClient::WatchedPad )
				Client->WatchPads();
			break;

		// Using a real pad puts the on-screen controls away and hands it the game.
		case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		case SDL_EVENT_GAMEPAD_AXIS_MOTION:
			if( USDLClient::WatchedPad && Event.gdevice.which == SDL_GetGamepadID( USDLClient::WatchedPad )
				&& (Event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || Abs( (INT)Event.gaxis.value ) > 16000) )
				SwitchVirtualJoystick( 0 );
			break;

		case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
			if( USDLClient::Gamepad && Event.gsensor.which == SDL_GetGamepadID( USDLClient::Gamepad ) )
				ProcessGyro( Event.gsensor.which, (SDL_SensorType)Event.gsensor.sensor, Event.gsensor.sensor_timestamp, Event.gsensor.timestamp, Event.gsensor.data );
			break;

		case SDL_EVENT_SENSOR_UPDATE:
			ProcessDeviceGyro( Event.sensor, Client->Window );
			break;

		case SDL_EVENT_GAMEPAD_REMOVED:
			if( USDLClient::WatchedPad && Event.gdevice.which == SDL_GetGamepadID( USDLClient::WatchedPad ) )
				Client->WatchPads();
			if( USDLClient::Gamepad && Event.gdevice.which == SDL_GetGamepadID( USDLClient::Gamepad ) )
			{
				Client->CloseGamepad();
				// Falls back to another connected pad, which sends no added event.
				if( Client->UseJoystick )
					Client->OpenGamepad();
				PadHeld = 0;
				if( GUIController )
					GUIController->ResetInput();
				Input->ResetInput();
			}
			break;

		default:;
	}
}

void USDLViewport::SendGyro()
{
	USDLClient* Client = GetOuterUSDLClient();
	// The JoyU and JoySlider2 binds turn degrees into view rotation, menus drop the turn.
	if( (GyroYaw || GyroPitch) && !(GUIController && GUIController->bActive) )
	{
		CauseInputEvent( IK_JoyU, IST_Axis, GyroYaw * Client->PadGyroSensitivity );
		CauseInputEvent( IK_JoySlider2, IST_Axis, GyroPitch * Client->PadGyroSensitivity );
	}
	GyroYaw = GyroPitch = 0.f;
}

void USDLViewport::SendMouse( FLOAT DX, FLOAT DY )
{
	USDLClient* Client = GetOuterUSDLClient();
    WindowsMouseX = Clamp<FLOAT>( WindowsMouseX + DX, 0, SizeX );
    WindowsMouseY = Clamp<FLOAT>( WindowsMouseY + DY, 0, SizeY );

	// Send to input subsystem.
	if( DX )
		CauseInputEvent( IK_MouseX, IST_Axis, +DX );

	if( DY )
		CauseInputEvent( IK_MouseY, IST_Axis, -DY );

	if (!Client->InMenuLoop)
		Client->Engine->MouseDelta(this, 0, appRound(DX), appRound(DY));

	if (!IsRealtime())
	{
		if( Input->KeyDown(IK_Space) )
		{
			for( INT i=0; i<Client->Viewports.Num(); i++ )
				Client->Viewports(i)->Repaint( 1 );
		}
		else
		{
			Repaint( 1 );
		}
	}
}

void USDLViewport::UpdateInput( UBOOL Reset, FLOAT DeltaSeconds )
{
	guard(USDLViewport::UpdateInput);

	// Resets come from SetRes and from inside the event loop below, so they leave the queue alone.
	if( Reset )
		return;

	USDLClient* Client = GetOuterUSDLClient();

	UpdateTap();

	UBOOL InMenu = GUIController && GUIController->bActive;
	// A vehicle outlives its driver, so NEXTSEAT starts over once the player has left it.
	if( !Actor || Actor->Pawn != LastSeatPawn )
		LastSeatPawn = NULL;
	UpdateGamepad( InMenu, DeltaSeconds );
	UpdateCursor( InMenu );
	UpdateTextInput( InMenu );
#if __IOS__
	IOSTouchFrame( InMenu );
#endif

	// Mouse movement management.
	UBOOL MouseMoved = false;
	UBOOL Resized = false;
	FLOAT DX = 0, DY = 0;

	SDL_Event Event;
	while( SDL_PollEvent( &Event ) )
	{
		switch( Event.type )
		{
            case SDL_EVENT_QUIT:
                GIsRequestingExit = 1;
                break;

			case SDL_EVENT_KEY_DOWN:
			case SDL_EVENT_KEY_UP:
				KeyEvent( Event.key );
				break;

			case SDL_EVENT_TEXT_INPUT:
			{
				const char* Text = Event.text.text;
				while( Uint32 Char = SDL_StepUTF8( &Text, NULL ) )
					if( Char < 0x10000 )
						Client->Engine->Key( this, Char < 256 ? (EInputKey)Char : IK_Unicode, (TCHAR)Char );
				break;
			}

			case SDL_EVENT_FINGER_DOWN:
			case SDL_EVENT_FINGER_MOTION:
			case SDL_EVENT_FINGER_UP:
			case SDL_EVENT_FINGER_CANCELED:
				if( VJShown && USDLClient::VirtualPad )
					VJPointer( Event.tfinger.touchID, Event.tfinger.fingerID, Event.type, Event.tfinger.x * SizeX, Event.tfinger.y * SizeY );
				// Touching the game brings back the controls a mouse or pad put away.
				else if( Event.type == SDL_EVENT_FINGER_DOWN && Client->VJWanted && !USDLClient::VirtualPad && !InMenu )
					SwitchVirtualJoystick( 1 );
				break;

			case SDL_EVENT_MOUSE_BUTTON_DOWN:
			case SDL_EVENT_MOUSE_BUTTON_UP:
				MouseButtonEvent( Event.button, InMenu, DX, DY );
				break;

			case SDL_EVENT_MOUSE_WHEEL:
				MouseWheelEvent( Event.wheel );
				break;

			case SDL_EVENT_MOUSE_MOTION:
				MouseMotionEvent( Event.motion, InMenu, MouseMoved, DX, DY );
				break;

			case SDL_EVENT_GAMEPAD_ADDED:
			case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
			case SDL_EVENT_GAMEPAD_AXIS_MOTION:
			case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
			case SDL_EVENT_SENSOR_UPDATE:
			case SDL_EVENT_GAMEPAD_REMOVED:
				GamepadEvent( Event );
				break;

			case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
				Resized = true;
				break;

			case SDL_EVENT_WINDOW_FOCUS_LOST:
				VJRelease();
				if( GUIController )
					GUIController->ResetInput();
				if( Input )
					Input->ResetInput();
				break;

			default:;
		}
	}

	// Once after the loop, so a drag resizes once per frame. The window already has the new size.
	if( Resized && RenDev && !(BlitFlags & BLIT_Fullscreen) )
	{
		int W, H;
		SDL_GetWindowSizeInPixels( Client->Window, &W, &H );
		if( W != SizeX || H != SizeY )
			ResizeViewport( BlitFlags | BLIT_NoWindowChange, W, H );
	}

	SendGyro();

	if( MouseMoved )
		SendMouse( DX, DY );

	unguard;
}



/*-----------------------------------------------------------------------------
	Lock and Unlock.
-----------------------------------------------------------------------------*/

//
// Lock the viewport window and set the approprite Screen and RealScreen fields
// of Viewport.  Returns 1 if locked successfully, 0 if failed.
//
UBOOL USDLViewport::Lock( BYTE* HitData, INT* HitSize )
{
	guard(USDLViewport::Lock);

	// The HUD adds the frame's targets as it draws.
	TouchTargets.Empty( TouchTargets.Num() );

	// Success here, so pass to superclass.
	return UViewport::Lock(HitData,HitSize);

	unguard;
}

//
// Unlock the viewport window.  If Blit=1, blits the viewport's frame buffer.
//
void USDLViewport::Unlock()
{
	guard(USDLViewport::Unlock);
	UViewport::Unlock();
	unguard;
}

/*-----------------------------------------------------------------------------
	Viewport modes.
-----------------------------------------------------------------------------*/

//
// Try switching to a new rendering device.
//
UBOOL USDLViewport::TryRenderDevice( const TCHAR* ClassName, INT NewX, INT NewY, UBOOL Fullscreen )
{
	guard(USDLViewport::TryRenderDevice);

	// Shut down current rendering device.
	guard(ShutdownOldRenDev);
	if( RenDev && RenDev != GetOuterUSDLClient()->Engine->GRenDev ) 
	{
		RenDev->Exit(this);
		delete RenDev;
		RenDev = NULL;
	}
	unguard;

	// Use appropriate defaults.
	USDLClient* C = GetOuterUSDLClient();
	if( NewX==INDEX_NONE )
		NewX = C->WindowedViewportX;
	if( NewY==INDEX_NONE )
		NewY = C->WindowedViewportY;

	// Find device driver.
	UClass* RenderClass = UObject::StaticLoadClass( URenderDevice::StaticClass(), NULL, ClassName, NULL, 0, NULL );
	if( RenderClass  )
	{
		if( RenderClass == GetOuterUSDLClient()->Engine->GRenDev->GetClass() )
		{
			RenDev = GetOuterUSDLClient()->Engine->GRenDev;
		}
		else
		{
			RenDev = ConstructObject<URenderDevice>( RenderClass, this );
			RenDev->Init();
		}

        // "ColorBytes" should be zero unless a fallback is needed.
		if( !RenDev->SetRes( this, NewX, NewY, Fullscreen | FullscreenOnly, ColorBytes ) )
		{
			debugf( NAME_Log, LocalizeError(TEXT("Failed3D"),TEXT("SDLDrv")) );
			if( RenDev != GetOuterUSDLClient()->Engine->GRenDev ) 
				delete RenDev;
			RenDev = NULL;
		}
	}

  	if( !GIsEditor )
	{
		if ( GUIController )
		{
			GUIController->ResetInput();
			GUIController->ResolutionChanged(SizeX,SizeY);
		}
		if ( Input )
			Input->ResetInput();
    }

    return RenDev != NULL;
	unguard;
}


//
// If in fullscreen mode, end it.
//
void USDLViewport::EndFullscreen()
{
	guard(USDLViewport::EndFullscreen);
	USDLClient* Client = GetOuterUSDLClient();
	debugf(TEXT("EndFullscreen"));
	if( RenDev && (BlitFlags & (BLIT_Direct3D|BLIT_OpenGL)) && !FullscreenOnly )
	{
		RenDev->SetRes( this, Client->WindowedViewportX, Client->WindowedViewportY, 0 );
	}
	else if( RenDev && (BlitFlags & BLIT_OpenGL) )
	{
		RenDev->SetRes( this, INDEX_NONE, INDEX_NONE, FullscreenOnly );
	}
	UpdateWindowFrame();

	if ( GUIController )
	{
		GUIController->ResetInput();
		GUIController->ResolutionChanged(SizeX, SizeY);
	}

	if ( Input )
		Input->ResetInput();

	unguard;
}

//
// Toggle fullscreen.
//
void USDLViewport::ToggleFullscreen()
{
	guard(USDLViewport::ToggleFullscreen);
	if( BlitFlags & BLIT_Fullscreen )
	{
		EndFullscreen();
		SetMouseCapture(1,1,0);
	}
	else if( !(Actor->ShowFlags & SHOW_ChildWindow) )
	{
		debugf(TEXT("AttemptFullscreen"));
		TryRenderDevice( TEXT("ini:Engine.Engine.RenderDevice"), INDEX_NONE, INDEX_NONE, 1 );
	}
	USDLClient* Client = GetOuterUSDLClient();
	Client->StartupFullscreen = IsFullscreen();
	Client->SaveConfig();
	unguard;
}

//
// SETRES WxH[w|f]. Fullscreen always covers the desktop, so the size only applies to the window.
//
void USDLViewport::SetResolution( const TCHAR* Cmd, UBOOL bSaveSize )
{
	guard(USDLViewport::SetResolution);
	INT X = appAtoi(Cmd);
	const TCHAR* Rest = appStrchr(Cmd,'x') ? appStrchr(Cmd,'x')+1 : appStrchr(Cmd,'X') ? appStrchr(Cmd,'X')+1 : TEXT("");
	INT Y = appAtoi(Rest);
	UBOOL Fullscreen = IsFullscreen() || FullscreenOnly;
	if( appStrchr(Rest,'w') || appStrchr(Rest,'W') )
		Fullscreen = 0;
	else if( appStrchr(Rest,'f') || appStrchr(Rest,'F') )
		Fullscreen = 1;
	if( !X || !Y )
		return;
	if( !RenDev->SetRes( this, X, Y, Fullscreen || FullscreenOnly, 0, bSaveSize ) )
		EndFullscreen();
	if( GUIController )
		GUIController->ResolutionChanged(SizeX, SizeY);
	unguard;
}

//
// Resize the viewport.
//
UBOOL USDLViewport::ResizeViewport( DWORD NewBlitFlags, INT InNewX, INT InNewY, UBOOL bSaveSize )
{
	guard(USDLViewport::ResizeViewport);
	USDLClient* Client = GetOuterUSDLClient();

	// BLIT_Temporary is kept so the temporary screen buffer doesn't leak during light rebuilds.

	// Remember viewport.
	UViewport* SavedViewport = NULL;
	if( Client->Engine->Audio && !GIsEditor && !(GetFlags() & RF_Destroyed) )
		SavedViewport = Client->Engine->Audio->GetViewport();

	// Fullscreen takes the desktop size below.
	INT NewX = InNewX!=INDEX_NONE ? InNewX : Client->WindowedViewportX;
	INT NewY = InNewX!=INDEX_NONE ? InNewY : Client->WindowedViewportY;

	check(NewX>=0);
	check(NewY>=0);

	if( !(NewBlitFlags & BLIT_Temporary) )
	{
		ScreenPointer = NULL;
	}

	// Set new info.
	BlitFlags = NewBlitFlags & ~BLIT_ParameterFlags;

#if defined(__ANDROID__) || defined(SDL_PLATFORM_IOS)
	// The launcher's splash window becomes the game window, phones have only one.
	if( !Client->Window )
	{
		SDL_Window** Windows = SDL_GetWindows( NULL );
		if( Windows && Windows[0] )
		{
			Client->Window = Windows[0];
			SDL_DestroyRenderer( SDL_GetRenderer( Windows[0] ) );
		}
		SDL_free( Windows );
	}
#endif
	if( !Client->Window )
	{
		// Resizable from the start, since in a browser SDL follows the page's canvas size only for such a window.
		Client->Window = SDL_CreateWindow( "", NewX, NewY, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN | (GIsEditor ? 0 : SDL_WINDOW_RESIZABLE) );
		if( !Client->Window )
			appErrorf( TEXT("Couldn't create window: %s"), appFromAnsi(SDL_GetError()) );

		// The menus need 640x480, a smaller window makes the GUI switch to a fixed resolution.
		if( !GIsEditor )
			SDL_SetWindowMinimumSize( Client->Window, 640, 480 );

#ifndef WIN32
		// Windows windows get the executable's icon.
		SDL_Surface* Icon = SDL_LoadBMP( "../Help/Unreal.bmp" );
		if( Icon )
		{
			SDL_SetSurfaceColorKey( Icon, true, SDL_MapSurfaceRGB( Icon, 255, 0, 255 ) );
			SDL_SetWindowIcon( Client->Window, Icon );
			SDL_DestroySurface( Icon );
		}
#endif
	}

	// A null render device may have opened the window first.
	if( (NewBlitFlags & BLIT_OpenGL) && !Client->GLContext )
	{
		Client->GLContext = SDL_GL_CreateContext( Client->Window );
		if( !Client->GLContext )
			appErrorf( TEXT("Couldn't create OpenGL context: %s"), appFromAnsi(SDL_GetError()) );
	}

	// User resizing already gave the window its size.
	SDL_Window* Window = Client->Window;
	if( !(NewBlitFlags & BLIT_NoWindowChange) )
	{
		if( NewBlitFlags & BLIT_Fullscreen )
		{
			// Borderless at the desktop size, so switching away is instant.
			SDL_SetWindowFullscreenMode( Window, NULL );
			SDL_SetWindowFullscreen( Window, true );
		}
		else
		{
			SDL_SetWindowFullscreen( Window, false );
#ifndef __EMSCRIPTEN__
			SDL_SetWindowSize( Window, Align(NewX,2), NewY );
#endif
			SDL_SetWindowResizable( Window, !GIsEditor );
		}
		SDL_ShowWindow( Window );
		SDL_SyncWindow( Window );

		// The OS may limit the window.
		int W, H;
		SDL_GetWindowSizeInPixels( Window, &W, &H );
		NewX = W;
		NewY = H;
	}
	SizeX = NewX;
	SizeY = NewY;

	SDL_HideCursor();
	WindowsMouseShown = 0;
	// Phones and tablets are always fullscreen and touched, so menus follow the absolute touch position.
	bWindowsMouseAvailable = !(NewBlitFlags & BLIT_Fullscreen) || SDL_HasScreenKeyboardSupport();

	SetTitleBar();

	// Make this viewport current and update its title bar.
	GetOuterUClient()->MakeCurrent( this );	

	// Update audio.
	if( SavedViewport && SavedViewport!=Client->Engine->Audio->GetViewport() )
		Client->Engine->Audio->SetViewport( SavedViewport );

	// Update the window.
	UpdateWindowFrame();

	// Save info.
	if( RenDev && !GIsEditor && bSaveSize && !(NewBlitFlags & BLIT_Fullscreen) && NewX && NewY )
	{
		Client->WindowedViewportX  = NewX;
		Client->WindowedViewportY  = NewY;
		Client->SaveConfig();
	}

	if( !(NewBlitFlags & BLIT_Fullscreen) )
		WantGrab = Client->CaptureMouse && !bShowWindowsMouse;
	UpdateMouseGrabState();

	return 1;
	unguard;
}

//
// Get localized keyname.
//
TCHAR * USDLViewport::GetLocalizedKeyName( EInputKey Key )
{
	return TEXT("");
}




void USDLViewport::TextToSpeech( const FString& Text, FLOAT Volume )
{
    INT len = Text.Len();
    if (len == 0)
        return;

    if (TextToSpeechObject != -1)
    {
        char *cvt = (char *) appAlloca(len + 2);
        if (cvt)
        {
            appToAnsi(*Text, cvt);
            if (cvt[len-1] != '\n')
            {
                cvt[len++] = '\n';
                cvt[len] = '\0';
            }

            // Could be made non-blocking.
            write(TextToSpeechObject, cvt, len);
        }
    }
}


/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/

