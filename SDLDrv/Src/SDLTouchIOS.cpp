/*=============================================================================
	SDLTouchIOS.cpp: UT2004's side of the iOS touch controls.

	The overlay (iOS/touch/IOSTouch.m, the owner's DOOM port's, the touch
	controls standard of all their ports) sits on top of SDL's view while a
	match is played and turns touches into the stick, the look, the buttons
	and MENU with its tray. This file tells it each frame what the game is
	doing and what the player carries, and feeds what it read into UT2004:
	  - the stick and the buttons through SDL's virtual gamepad, which UT2004's
	    pad bindings already turn into movement, Fire, Jump and the rest;
	  - the look and tilt aiming as degrees on IK_JoyU / IK_JoySlider2, the
	    axes the pad's gyro uses (Axis aBaseX / aLookUp, Speed=28.44);
	  - NEXT WPN, PREV WPN and the weapon wheel's pick as NextWeapon,
	    PrevWeapon and GetWeapon;
	  - MENU as Escape, the tray's keyboard as the console key, RESUME as Pause.
	DOOM's buttons map onto UT2004's this way (IOSTouch.m's labels match):
	  FIRE: Fire; the secondary button (IOSTOUCH_CROUCH): ALT FIRE; the arc's
	  JUMP: Jump; its other button (IOSTOUCH_USE): CROUCH; MAP's tap
	  (IOSTOUCH_REQ_MAP): USE.
=============================================================================*/

#include "SDLDrv.h"

#if __IOS__

#pragma pack(push,8)
#include "IOSTouch.h"
#pragma pack(pop)

// The overlay's look comes in DOOM's mouse counts, 0.17578 degrees each (IOSTouch.m's IOSTOUCH_LOOK_SCALE).
static const FLOAT DegreesPerCount = 0.17578f;

// UT2004 has no levels list: the overlay's LEVEL is hidden, these are only linked.
extern "C" int IOSGame_LevelInfo( int iLevel, char* strName, int ctName, char* strFile, int ctFile ) { return 0; }
extern "C" int IOSGame_CurrentLevel( void ) { return 0; }

// The weapons the overlay last saw, so the wheel's pick finds its class.
static UClass* GWheelClasses[IOSTOUCH_WPN_MAX];

static INT WeaponAmmo( AWeapon* W, INT& Max )
{
	Max = 0;
	UClass* AmmoClass = W->AmmoClass[0];
	if( !AmmoClass )
		return -1;
	AAmmunition* Default = Cast<AAmmunition>( AmmoClass->GetDefaultObject() );
	if( Default )
		Max = Default->MaxAmmo;
	if( W->Ammo[0] )
		return W->Ammo[0]->AmmoAmount;
	return W->AmmoCharge[0];
}

// Slice colours by the weapon's slot (UT2004's InventoryGroup).
static int WeaponColour( BYTE Group )
{
	switch( Group )
	{
		case 2: case 6: return IOSTOUCH_WCOL_BULLETS;		// assault rifle, minigun
		case 3: return IOSTOUCH_WCOL_NAPALM;				// bio rifle
		case 4: case 5: return IOSTOUCH_WCOL_ELECTRICITY;	// shock rifle, link gun
		case 7: return IOSTOUCH_WCOL_SHELLS;				// flak cannon
		case 8: return IOSTOUCH_WCOL_ROCKETS;				// rocket launcher
		case 9: return IOSTOUCH_WCOL_SNIPER;				// lightning gun, sniper rifle
		case 0: case 10: return IOSTOUCH_WCOL_GRENADES;		// super weapons, grenade launchers
		default: return IOSTOUCH_WCOL_NONE;					// shield gun, translocator
	}
}

// What the player carries, in NextWeapon's order (slot, then the order within the slot).
static void ListWeapons( APawn* Pawn, IOSTouchWeapons& L )
{
	appMemzero( &L, sizeof(L) );
	if( !Pawn )
		return;
	L.bValid = 1;
	L.bCanSelect = Pawn->Health > 0;
	L.iWanted = -1;

	AWeapon* Found[IOSTOUCH_WPN_MAX];
	INT Count = 0;
	INT Guard = 0;
	for( AInventory* Inv = Pawn->Inventory; Inv && Guard < 200; Inv = Inv->Inventory, Guard++ )
	{
		AWeapon* W = Cast<AWeapon>( Inv );
		if( !W || Count == IOSTOUCH_WPN_MAX )
			continue;
		INT At = Count++;
		while( At > 0 && (Found[At-1]->InventoryGroup > W->InventoryGroup
			|| (Found[At-1]->InventoryGroup == W->InventoryGroup && Found[At-1]->GroupOffset > W->GroupOffset)) )
		{
			Found[At] = Found[At-1];
			At--;
		}
		Found[At] = W;
	}

	AWeapon* Wanted = Pawn->PendingWeapon ? Pawn->PendingWeapon : Pawn->Weapon;
	DWORD Hash = 2166136261u;
	for( INT i=0; i<Count; i++ )
	{
		AWeapon* W = Found[i];
		GWheelClasses[i] = W->GetClass();
		Hash = (Hash ^ (DWORD)(PTRINT)W->GetClass()) * 16777619u;
		FString Name = W->ItemName.Len() ? W->ItemName : FString(W->GetName());
		strncpy( L.astrName[i], appToAnsi(*Name), IOSTOUCH_WPN_NAME - 1 );
		L.astrName[i][IOSTOUCH_WPN_NAME - 1] = 0;
		L.aiColour[i] = WeaponColour( W->InventoryGroup );
		INT Max;
		L.aiAmmo[i] = WeaponAmmo( W, Max );
		L.aiMaxAmmo[i] = Max;
		L.ulOwned |= 1u << i;
		if( L.aiAmmo[i] != 0 )
			L.ulReady |= 1u << i;
		if( W == Wanted )
			L.iWanted = i;
	}
	L.ctWeapons = Count;
	L.uList = Hash ? Hash : 1;
}

// Whether an object's script is in this state.
static UBOOL InState( UObject* Object, FName State )
{
	FStateFrame* Frame = Object ? Object->GetStateFrame() : NULL;
	return Frame && Frame->StateNode && Frame->StateNode->GetFName() == State;
}

// Fingers lifted off the screen, counted by MouseButtonEvent.
DWORD GIOSScreenTaps = 0;

// Whether an object is of a script class with this name, or one under it.
static UBOOL IsClassNamed( UObject* Object, const TCHAR* Name )
{
	for( UClass* Class = Object ? Object->GetClass() : NULL; Class; Class = Class->GetSuperClass() )
		if( appStricmp( Class->GetName(), Name ) == 0 )
			return 1;
	return 0;
}

// A key pressed and let go, as a bound key would be.
static void TapKey( USDLViewport* Viewport, EInputKey Key )
{
	Viewport->CauseInputEvent( Key, IST_Press );
	Viewport->CauseInputEvent( Key, IST_Release );
}

void USDLViewport::IOSTouchFrame( UBOOL InMenu )
{
	guard(USDLViewport::IOSTouchFrame);
	USDLClient* Client = GetOuterUSDLClient();
	static unsigned int FramesDrawn = 0;
	static UBOOL PadGyroOff = 0;
	static UBOOL WasNext = 0, WasPrev = 0;
	static INT LastPick = -1, UseFrames = 0;

	// Tilt aiming is the overlay's GYRO (off by default), not the pad's.
	if( !PadGyroOff )
	{
		PadGyroOff = 1;
		Client->PadGyro = 0;
		Client->EnableGyro();
	}

	APlayerController* PC = Actor;
	APawn* Pawn = PC ? PC->Pawn : NULL;
	// The console typing, or ExtendedConsole's full console (as UpdateTextInput tells them).
	static FName NAME_ConsoleVisible( TEXT("ConsoleVisible") );
	FStateFrame* ConsoleState = Console ? Console->GetStateFrame() : NULL;
	UBOOL ConsoleOpen = Console && Console->IsA(UConsole::StaticClass())
		&& (((UConsole*)Console)->bTyping || (ConsoleState && ConsoleState->StateNode && ConsoleState->StateNode->GetFName() == NAME_ConsoleVisible));
	// The intros (CinematicPlayer): no controls, and a tap skips to the menu as a click would.
	static DWORD TapsSeen = 0;
	UBOOL Intro = PC && IsClassNamed( PC, TEXT("CinematicPlayer") );
	if( Intro && !InMenu && GIOSScreenTaps != TapsSeen )
		Exec( TEXT("SHOWMENU"), *GLog );
	TapsSeen = GIOSScreenTaps;
	int Mode = IOSTOUCH_HIDDEN;
	if( !InMenu && !Intro && PC && Client->Window )
	{
		static FName Dead( TEXT("Dead") ), Waiting( TEXT("PlayerWaiting") ), Spectating( TEXT("Spectating") ), WaitingForPawn( TEXT("WaitingForPawn") );
		UBOOL Playing = Pawn || InState(PC, Dead) || InState(PC, Waiting) || InState(PC, Spectating) || InState(PC, WaitingForPawn);
		if( ConsoleOpen )
			Mode = IOSTOUCH_CONSOLE;
		else if( PC->Level && PC->Level->Pauser )
			Mode = IOSTOUCH_PAUSED;
		else if( Playing )
			Mode = IOSTOUCH_GAMEPLAY;
	}

	IOSTouchWeapons Weapons;
	ListWeapons( Mode == IOSTOUCH_GAMEPLAY ? Pawn : NULL, Weapons );
	IOSTouch_SetWeapons( &Weapons );
	// No icons yet: the slices show the weapons' names.
	for( unsigned int Wanted = IOSTouch_WantedIcons(), i = 0; Wanted; Wanted >>= 1, i++ )
		if( Wanted & 1 )
			IOSTouch_SetWeaponIcon( i, NULL, 0, 0 );

	IOSTouchHud Hud;
	appMemzero( &Hud, sizeof(Hud) );
	Hud.bValid = 1;
	Hud.bCanJump = 1;
	Hud.bCanCrouch = 1;
	Hud.bNoSaves = 1;
	Hud.bRunOn = 1;		// UT2004 always runs: no run marker
	Hud.uWorld = (unsigned int)(PTRINT)(PC ? PC->Level : NULL);
	int Requests = IOSTouch_Update( Client->Window, Mode, &Hud, ++FramesDrawn );

	if( Requests & IOSTOUCH_REQ_MENU )
		TapKey( this, IK_Escape );
	if( Requests & IOSTOUCH_REQ_CONSOLE )
		TapKey( this, IK_Tilde );
	if( (Requests & IOSTOUCH_REQ_RESUME) && PC )
		Exec( TEXT("PAUSE"), *GLog );
	if( Requests & IOSTOUCH_REQ_MAP )
		UseFrames = 2;

	IOSTouchInput In;
	IOSTouch_ReadInput( &In );
	SDL_Joystick* Joystick = USDLClient::VirtualPad ? SDL_GetGamepadJoystick( USDLClient::Gamepad ) : NULL;
	UBOOL Play = Mode == IOSTOUCH_GAMEPLAY;
	if( Joystick )
	{
		FLOAT MoveX = Play ? Clamp( In.fMoveX, -1.f, 1.f ) : 0.f, MoveY = Play ? Clamp( In.fMoveY, -1.f, 1.f ) : 0.f;
		SDL_SetJoystickVirtualAxis( Joystick, SDL_GAMEPAD_AXIS_LEFTX, (Sint16)(MoveX * 32767.f) );
		// The pad's stick is down for +, the overlay's forward is +.
		SDL_SetJoystickVirtualAxis( Joystick, SDL_GAMEPAD_AXIS_LEFTY, (Sint16)(-MoveY * 32767.f) );
		UBOOL Fire = Play && (In.ulButtons & IOSTOUCH_FIRE), AltFire = Play && (In.ulButtons & IOSTOUCH_CROUCH);
		SDL_SetJoystickVirtualAxis( Joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, Fire ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN );
		SDL_SetJoystickVirtualAxis( Joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, AltFire ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN );
		// The pad's buttons: 0 Jump, 1 Duck, 2 Use (Joy1..3 in User.ini).
		SDL_SetJoystickVirtualButton( Joystick, 0, Play && (In.ulButtons & IOSTOUCH_JUMP) );
		SDL_SetJoystickVirtualButton( Joystick, 1, Play && (In.ulButtons & IOSTOUCH_USE) );
		SDL_SetJoystickVirtualButton( Joystick, 2, Play && UseFrames > 0 );
	}
	if( UseFrames > 0 )
		UseFrames--;

	if( Play )
	{
		// A tap is reported for two reads, so only its start steps once.
		UBOOL Next = (In.ulButtons & IOSTOUCH_NEXTWEAPON) != 0, Prev = (In.ulButtons & IOSTOUCH_PREVWEAPON) != 0;
		if( Next && !WasNext )
			Exec( TEXT("NEXTWEAPON"), *GLog );
		if( Prev && !WasPrev )
			Exec( TEXT("PREVWEAPON"), *GLog );
		WasNext = Next;
		WasPrev = Prev;

		if( In.iSelectWeapon >= 0 && In.iSelectWeapon < Weapons.ctWeapons && In.iSelectWeapon != LastPick && GWheelClasses[In.iSelectWeapon] )
			Exec( *FString::Printf( TEXT("GETWEAPON %s"), GWheelClasses[In.iSelectWeapon]->GetPathName() ), *GLog );
		LastPick = In.iSelectWeapon;

		// The look and tilt aiming in degrees: + turns right and looks up.
		FLOAT DX = 0.f, DY = 0.f, Yaw = 0.f, Pitch = 0.f;
		IOSTouch_TakeLook( &DX, &DY );
		IOSTouch_TakeGyro( &Yaw, &Pitch );
		FLOAT Turn = DX * DegreesPerCount - Yaw, LookUp = -DY * DegreesPerCount + Pitch;
		if( Turn )
			CauseInputEvent( IK_JoyU, IST_Axis, Turn );
		if( LookUp )
			CauseInputEvent( IK_JoySlider2, IST_Axis, LookUp );
	}
	else
	{
		WasNext = WasPrev = 0;
		LastPick = -1;
	}
	unguard;
}

#endif
