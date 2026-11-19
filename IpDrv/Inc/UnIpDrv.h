/*=============================================================================
	IpDrvPrivate.h: Unreal TCP/IP driver.
	Copyright 1997-1999 Epic Games, Inc. All Rights Reserved.

Revision history:
	* Created by Tim Sweeney.
=============================================================================*/

#ifndef UNIPDRV_H
#define UNIPDRV_H

#if _MSC_VER
	#pragma warning( disable : 4201 )
#endif

// Socket API.
#if _MSC_VER
	#define __WINSOCK__ 1
	#define SOCKET_API TEXT("WinSock")
#else
	#define __BSD_SOCKETS__ 1
	#define SOCKET_API TEXT("Sockets")
#endif

// WinSock includes.
#if __WINSOCK__
#pragma pack(push,8)
	#include <windows.h>
	#include <winsock.h>
	#include <conio.h>
#pragma pack(pop)
#endif

// BSD socket includes.
#if __BSD_SOCKETS__
	#include <stdio.h>
	#include <unistd.h>
	#include <sys/types.h>
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <arpa/inet.h>
	#include <netdb.h>
	#include <sys/uio.h>
	#include <sys/ioctl.h>
	#include <sys/time.h>
	#include <errno.h>
	#include <pthread.h>
	#include <fcntl.h>

	// Apple has no such flag and 0x4000 means something else there. The process ignores SIGPIPE anyway.
	#if __APPLE__ && !defined(MSG_NOSIGNAL)
	#define MSG_NOSIGNAL 0
	#endif

	// Handle glibc < 2.1.3
	#ifndef MSG_NOSIGNAL
	#define MSG_NOSIGNAL 0x4000
	#endif

	#if __EMSCRIPTEN__
	// Emscripten emulates sockets over WebSockets, every ping, link and download would turn into a ws:// connection attempt.
	#define socket( Domain, Type, Protocol ) INVALID_SOCKET
	#endif
#endif

/*----------------------------------------------------------------------------
	API.
----------------------------------------------------------------------------*/

#ifndef IPDRV_API
	#define IPDRV_API DLL_IMPORT
#endif

/*-----------------------------------------------------------------------------
	Includes..
-----------------------------------------------------------------------------*/

#include "Engine.h"
#include "UnNet.h"
#include "UnSocket.h"

/*-----------------------------------------------------------------------------
	Definitions.
-----------------------------------------------------------------------------*/

// Globals.
extern UBOOL GIpDrvInitialized;

/*-----------------------------------------------------------------------------
	More Includes.
-----------------------------------------------------------------------------*/

#include "InternetLink.h"

/*-----------------------------------------------------------------------------
	Host resolution thread.
-----------------------------------------------------------------------------*/

#if __UNIX__
void* ResolveThreadEntry( void* Arg );
#else
DWORD STDCALL ResolveThreadEntry( void* Arg );
#endif

//
// Class for creating a background thread to resolve a host.
//
class FResolveInfo
{
public:
	// Variables.
	in_addr		Addr;
	INT			Done;
	ANSICHAR	HostName[256];
	TCHAR		Error[256];

	#if __UNIX__
	pthread_t	ResolveThread;
	#endif

	// Functions.
	FResolveInfo( const TCHAR* InHostName )
	{	
		debugf( TEXT("Resolving %s..."), InHostName );

		// The name can come from a server or an HTTP redirect.
		if( appStrlen(InHostName) >= ARRAY_COUNT(HostName) )
		{
			appStrncpy( Error, TEXT("Host name too long"), ARRAY_COUNT(Error) );
			Done = 1;
			return;
		}
		appMemcpy( HostName, appToAnsi(InHostName), appStrlen(InHostName) + 1 );
		*Error = 0;
		Done = 0;

#if __EMSCRIPTEN__
		// No threads, and Emscripten's resolver only makes up addresses for its WebSocket emulation.
		appStrncpy( Error, TEXT("Browsers can't look up host names"), ARRAY_COUNT(Error) );
		Done = 1;
#elif _MSC_VER
		HANDLE hThread = CreateThread( NULL, 0, ResolveThreadEntry, this, 0, NULL );
		check(hThread);
		CloseHandle( hThread );
#else
		pthread_attr_t ThreadAttributes;
		pthread_attr_init( &ThreadAttributes );
		pthread_attr_setdetachstate( &ThreadAttributes, PTHREAD_CREATE_DETACHED );
		pthread_create( &ResolveThread, &ThreadAttributes, &ResolveThreadEntry, this );
#endif

	}
	// Done is written by the resolve thread.
	UBOOL Resolved()
	{
#if _MSC_VER
		return InterlockedCompareExchange( (volatile LONG*)&Done, 0, 0 ) != 0;
#else
		return __atomic_load_n( &Done, __ATOMIC_ACQUIRE ) != 0;
#endif
	}
	const TCHAR* GetError()
	{
		return *Error ? Error : NULL;
	}
	const in_addr GetAddr()
	{
		return Addr;
	}
	const TCHAR* GetHostName()
	{
		return appFromAnsi(HostName);
	}
};

/*-----------------------------------------------------------------------------
	Bind to next available port.
-----------------------------------------------------------------------------*/

//
// Bind to next available port.
//
inline int bindnextport( SOCKET s, struct sockaddr_in* addr, int portcount, int portinc )
{
	guard(bindnextport);

#ifdef __linux__
    // Long explanation follows:
    //
    // Using port 0 tells the OS to allocate an unused port. But, this will
    // allocate the same port as the socket we just closed on Linux's
    // TCP/IP stack. If you get a stray packet from the last socket, the game
    // gets VERY confused. This is most obvious by hitting a passworded server
    // with the wrong password. We connect, fail to gain access, user enters a
    // password and we closesocket() and immediately hit the server again with
    // a newly bound port...and gets some extra unexpected chatter that hangs
    // the client.
    //
    // The workaround is to pick our own random ports if port zero is
    // requested. If we can't find a valid port above 1024 (below 1024 is
    // seconds and then continue as normal, which tends to fix things.
    // tends to fix things, too. Oh well.
    //
    // The Win32 TCP/IP stack does not exhibit this problem.

    if( addr->sin_port==0 )
    {
        // try 10 random ports...
        for (int i = 0; i < 10; i++)
        {
            short p = ((short) (31742.0f * rand() / (RAND_MAX + 1.0))) + 1024;
            addr->sin_port = htons(p);
            int rc = bindnextport(s, addr, portcount, portinc);
            if (rc != 0)
                return(rc);  // got one.
        }

        debugf( NAME_Warning, TEXT("Failed to manually bind random port. Letting OS do it instead.") );
        appSleep(3.0f);
        addr->sin_port = 0;
    }
#endif




	for( int i=0; i<portcount; i++ )
	{
		if( !bind( s, (sockaddr*)addr, sizeof(sockaddr_in) ) )
		{
			if (ntohs(addr->sin_port) != 0)
				return ntohs(addr->sin_port);
			else
			{
				// 0 means allocate a port for us, so find out what that port was
				struct sockaddr_in boundaddr;
				SOCKLEN size = sizeof(boundaddr);
				getsockname ( s, (sockaddr*)(&boundaddr), &size);
				return ntohs(boundaddr.sin_port);
			}
		}
		if( addr->sin_port==0 )
			break;
		addr->sin_port = htons( ntohs(addr->sin_port) + portinc );
	}
	return 0;
	unguard;
}

inline int getlocalhostaddr( FOutputDevice& Out, in_addr &HostAddr )
{
	guard(getlocalhostaddr);
	int CanBindAll = 0;
	IpSetInt( HostAddr, INADDR_ANY );
#if __EMSCRIPTEN__
	// A page has no address of its own.
	return CanBindAll;
#endif
	TCHAR Home[256]=TEXT("");
	TCHAR HostName[256]=TEXT("");
	ANSICHAR AnsiHostName[256]="";
	if( gethostname( AnsiHostName, 256 ) )
		Out.Logf( TEXT("%s: gethostname failed (%s)"), SOCKET_API, SocketError() );
	appStrcpy( HostName, appFromAnsi(AnsiHostName) );
	if( Parse(appCmdLine(),TEXT("MULTIHOME="),Home,ARRAY_COUNT(Home)) )
	{
		TCHAR *A, *B, *C, *D;
		A=Home;
		if
		(	(A=Home)!=NULL
		&&	(B=appStrchr(A,'.'))!=NULL
		&&	(C=appStrchr(B+1,'.'))!=NULL
		&&	(D=appStrchr(C+1,'.'))!=NULL )
		{
			IpSetBytes( HostAddr, appAtoi(A), appAtoi(B+1), appAtoi(C+1), \
				appAtoi(D+1) );
		}
		else Out.Logf( TEXT("Invalid multihome IP address %s"), Home );
	}
	else
	{
		check(gethostbyname_mutex != NULL);
		gethostbyname_mutex->Lock();
		HOSTENT* HostEnt = gethostbyname( appToAnsi(HostName) );
		if( HostEnt==NULL )
		{
			Out.Logf( TEXT("gethostbyname failed (%s) ... can't get local IP."), SocketHostError() );
		}
		else if( HostEnt->h_addrtype!=PF_INET )
		{
			Out.Logf( TEXT("gethostbyname: non-Internet address (%s)"), \
				SocketHostError() );
		}
		else
		{
			HostAddr = *(in_addr*)( *HostEnt->h_addr_list );
			if( !ParseParam(appCmdLine(),TEXT("PRIMARYNET")) )
				CanBindAll = 1;
			static UBOOL First=0;
			if( !First )
			{
				First = 1;
				debugf( NAME_Init, TEXT("%s: I am %s (%s)"), SOCKET_API, HostName, *IpString( HostAddr ) );
			}
		}
		gethostbyname_mutex->Unlock();
	}
	return CanBindAll;
	unguard;
}

//
// Get local IP to bind to
//
inline in_addr getlocalbindaddr( FOutputDevice& Out )
{
	guard(getlocalbindaddr);

	in_addr BindAddr;

	// If we can bind to all addresses, return 0.0.0.0
	if( getlocalhostaddr( Out, BindAddr ) )
		IpSetInt( BindAddr, INADDR_ANY );	
	return BindAddr;

	unguard;
}

inline UBOOL HasServerDetail( const TArray<FKeyValuePair>& Info, const TCHAR* Key )
{
	for( INT i=0; i<Info.Num(); i++ )
		if( Info(i).Key==Key )
			return 1;
	return 0;
}

inline FString GetServerDetail( const TArray<FKeyValuePair>& Info, const TCHAR* Key )
{
	for( INT i=0; i<Info.Num(); i++ )
		if( Info(i).Key==Key )
			return Info(i).Value;
	return FString();
}

// Replaces a server detail, adds it when missing and removes it when Value is empty
inline void SetServerDetail( TArray<FKeyValuePair>& Info, const TCHAR* Key, const FString& Value )
{
	INT i;
	for( i=0; i<Info.Num() && Info(i).Key!=Key; i++ );
	if( !Value.Len() )
	{
		if( i<Info.Num() )
			Info.Remove( i );
		return;
	}
	if( i==Info.Num() )
		Info(Info.AddZeroed()).Key = Key;
	Info(i).Value = Value;
}

// Started with -webrtc-client-only: no UDP game socket and no main masters, it joins and hosts over WebRTC only
inline UBOOL WebRtcOnly()
{
	return ParseParam( appCmdLine(), TEXT("webrtc-client-only") );
}

FString WebMasterURL();
void WebMasterAdvertise( const FServerResponseLine& Server );
// The Done functions return false while the request is running
struct FWebMasterRequest* WebMasterList();
UBOOL WebMasterListDone( struct FWebMasterRequest* Request, UBOOL& Ok, TArray<FServerResponseLine>& Servers, FString& News );
// Sends a client's offer to a browser host and gets its answer back
struct FWebMasterRequest* WebMasterOffer( const FString& HostId, const FString& Sdp );
UBOOL WebMasterOfferDone( struct FWebMasterRequest* Request, UBOOL& Ok, FString& Sdp );
void WebMasterRequestFree( struct FWebMasterRequest* Request );
// The relay for joining a browser host that can't be reached directly, an empty Host when the web master gives none.
// The strings are valid until the next call.
struct FWebMasterTurn
{
	const ANSICHAR *Host, *Username, *Password;
	INT Port;
};
// Returns false while the web master is being asked
UBOOL WebMasterTurn( FWebMasterTurn& Out );

// Writes the server's reply packets to a server browser query, nothing while no server runs
void ServerQueryAnswer( BYTE Command, FArchive& ArSend );
// Server browser queries over a WebRTC data channel, for servers and clients that UDP can't serve.
// Open gives 0 for unusable details, Send 0 while the channel opens, Recv 0 for nothing waiting and -1 once closed.
INT WebRtcQueryOpen( const FString& Host, const FString& Details );
UBOOL WebRtcQuerySend( INT Id, const BYTE* Data, INT Count );
INT WebRtcQueryRecv( INT Id, BYTE* Data, INT Max );
void WebRtcQueryClose( INT Id );
void AddMasterServers( class AMasterServerLink* Link, FArchive& Ar );

/*-----------------------------------------------------------------------------
	Public includes.
-----------------------------------------------------------------------------*/

#include "IpDrvClasses.h"
#include "HTTPDownload.h"

#if __STATIC_LINK
#define NAMES_ONLY
#define NATIVE_DEFS_ONLY
#define AUTOGENERATE_NAME(name)
#define AUTOGENERATE_FUNCTION(cls,idx,name)
#include "IpDrvClasses.h"
#undef AUTOGENERATE_FUNCTION
#undef AUTOGENERATE_NAME
#undef NATIVE_DEFS_ONLY
#undef NAMES_ONLY
#endif

#endif // UNIPDRV_H

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/

