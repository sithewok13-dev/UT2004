/*=============================================================================
	UnIX: Unix port of UnVcWin32.cpp.
	Copyright 1997-1999 Epic Games, Inc. All Rights Reserved.

	Revision history:
		* Cloned by Mike Danylchuk
		* Severely amputated and mutilated by Brandon Reinhart
		* Surgically altered by Jack Porter
		* Mangled and rehabilitated by Brandon Reinhart
		* Obfuscated by Daniel Vogel
		* Peed on by Ryan C. Gordon
=============================================================================*/
#if __GNUC__

// Standard includes.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <float.h>
#include <time.h>
#include <errno.h>

// System includes.
#include <sys/param.h>
#include <unistd.h>
#include <pwd.h>
#include <dirent.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/resource.h>

#include <dlfcn.h>

#include <netdb.h>

#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <spawn.h>

#if __FILE_NOCASE
#include <dirent.h>
#endif

// Bionic has backtrace only from API 33, Emscripten has none.
#if !__ANDROID__ && !__EMSCRIPTEN__
#include <execinfo.h>
#endif

#if __EMSCRIPTEN__
#include <emscripten.h>
#endif

// Core includes.
#include "CorePrivate.h"

// rcg04232002 Just to make sure this is sane...
#if defined(NO_UNICODE_OS_SUPPORT)
#  if defined(UNICODE)
#    undef UNICODE
#    define UNICODE 0
#  endif
#endif

#if UNICODE
#include <locale.h>
#include <langinfo.h>
#endif

#include "vorbis/codec.h"
#include "vorbis/vorbisfile.h"

/*-----------------------------------------------------------------------------
	Globals
-----------------------------------------------------------------------------*/

// Module
ANSICHAR GModule[32];

// Environment
extern char **environ;

TCHAR *GUnixSpawnOnExit = NULL;

HINSTANCE hInstance = 0;


#if UNICODE
CORE_API void unicode_str_to_stdout(const UNICHAR *str)
{
    if (str == NULL)
        str = TEXT("(null)");

    printf("UNICODE str: ");
    for (int i = 0; str[i]; i++)
        printf("%c", (ANSICHAR) str[i]);
    printf("\n");
}


CORE_API ANSICHAR* unixToANSI( ANSICHAR* ACh, const UNICHAR* InUCh )
{
	guardSlow(unixToANSI);
	if(InUCh)
	{
        INT Index;
		for(Index = 0;InUCh[Index] != 0;Index++)
		{
			ACh[Index] = (ANSICHAR) InUCh[Index];
		}
        ACh[Index] = 0;
		return ACh;
	}
	else
	{
		ACh[0] = 0;
		return ACh;
	}
	unguardSlow;
}


CORE_API INT unixGetSizeANSI( const UNICHAR* InUCh )
{
	guardSlow(unixGetSizeANSI);
	if(InUCh)
		return (wcslen(InUCh) + 1) * sizeof(ANSICHAR);
	else
		return sizeof(ANSICHAR);
	unguardSlow;
}


CORE_API UNICHAR* unixToUNICODE( UNICHAR* UCh, const ANSICHAR* InACh )
{
	guardSlow(unixToUNICODE);
	if(InACh)
	{
        INT Index;
		for(Index = 0;InACh[Index] != 0;Index++)
        {
			UCh[Index] = (UNICHAR) InACh[Index];
        }
        UCh[Index] = 0;
		return UCh;
	}
	else
	{
		UCh[0] = 0;
		return UCh;
	}
    unguardSlow;
}


CORE_API INT unixGetSizeUNICODE( const ANSICHAR* InACh )
{
	guardSlow(unixGetSizeUNICODE);
	if(InACh)
		return (strlen(InACh) + 1) * sizeof(UNICHAR);
	else
		return sizeof(UNICHAR);
    unguardSlow;
}


CORE_API UNICHAR* unixANSIToUNICODE(char* str)
{
	guardSlow(unixANSIToUnicode);
	INT iLength = unixGetSizeUNICODE(str);
	UNICHAR* pBuffer = new UNICHAR[iLength];
	appStrcpy(pBuffer,TEXT(""));
	return unixToUNICODE(pBuffer,str);
    unguardSlow;
}


CORE_API void unixDetectUNICODE( void )
{
    guard(unixDetectUNICODE);

    unguard;
}
#endif


#if UNICODE_BY_HAND
CORE_API UNICHAR* wcscpy( UNICHAR* _Dest, const UNICHAR* Src)
{
    guardSlow(wcscpy)

    UNICHAR *Dest = _Dest;

    while (*Src)
    {
        *Dest = *Src;
        Dest++;
        Src++;
    }

    *Dest = 0;

    return(_Dest);

    unguardSlow;
}


CORE_API UNICHAR* wcsncpy( UNICHAR* _Dest, const UNICHAR* Src, INT MaxLen )
{
    guardSlow(wcsncpy)

    UNICHAR *Dest = _Dest;

    if (MaxLen <= 0)
        return(Dest);

    MaxLen--;  // always leave room for null char.
    for (INT i = 0; ((i < MaxLen) && (*Src)); i++)
    {
        *Dest = *Src;
        Dest++;
        Src++;
    }

    *Dest = 0;

    return(Dest);

    unguardSlow;
}


CORE_API INT wcslen( const UNICHAR* String )
{
    guardSlow(wcslen);

    INT retval = 0;
    for (retval = 0; *String; String++)
        retval++;

    return(retval);

    unguardSlow;
}


CORE_API INT wcscmp( const UNICHAR* Str1, const UNICHAR *Str2 )
{
    guardSlow(wcscmp);

    while (true)
    {
        UNICHAR ch1 = *Str1;
        UNICHAR ch2 = *Str2;

        if ((ch1 == 0) && (ch1 == ch2))
            return(0);

        if (ch1 < ch2)
            return(-1);
        else if (ch1 > ch2)
            return(1);

        Str1++;
        Str2++;
    }

    return(1);  // shouldn't ever hit this.

    unguardSlow;
}


CORE_API INT wcsncmp( const UNICHAR* Str1, const UNICHAR *Str2, INT max )
{
    guardSlow(wcscmp);

    INT i = 0;
    while (true)
    {
        UNICHAR ch1 = *Str1;
        UNICHAR ch2 = *Str2;

        if (i == max)
            return(0);

        if ((ch1 == 0) && (ch1 == ch2))
            return(0);

        if (ch1 < ch2)
            return(-1);
        else if (ch1 > ch2)
            return(1);

        i++;
        Str1++;
        Str2++;
    }

    return(1);  // shouldn't ever hit this.

    unguardSlow;
}


CORE_API INT _wcsicmp( const UNICHAR* String1, const UNICHAR* String2 )
{
    guardSlow(_wcsicmp);

    while (true)
    {
        int ch1 = toupper((int) *String1);
        int ch2 = toupper((int) *String2);

        if ((ch1 == 0) && (ch1 == ch2))
            return(0);

        if (ch1 < ch2)
            return(-1);
        else if (ch1 > ch2)
            return(1);

        String1++;
        String2++;
    }

    return(1);  // shouldn't ever hit this.

    unguardSlow;
}



CORE_API INT _wcsnicmp( const UNICHAR* Str1, const UNICHAR *Str2, INT max )
{
    guardSlow(_wcsnicmp);

    INT i = 0;
    while (true)
    {
        UNICHAR ch1 = toupper((int) *Str1);
        UNICHAR ch2 = toupper((int) *Str2);

        if (i == max)
            return(0);

        if ((ch1 == 0) && (ch1 == ch2))
            return(0);

        if (ch1 < ch2)
            return(-1);
        else if (ch1 > ch2)
            return(1);

        i++;
        Str1++;
        Str2++;
    }

    return(1);  // shouldn't ever hit this.

    unguardSlow;
}


CORE_API UNICHAR* wcschr( const UNICHAR* String, const UNICHAR Find )
{
    guardSlow(wcschr);

    while (*String)
    {
        if (*String == Find)
            return((UNICHAR *) String);

        String++;
    }

    return(NULL);

    unguardSlow;
}


CORE_API UNICHAR* wcsstr( const UNICHAR* String, const UNICHAR* Find )
{
    guardSlow(wcsstr);
    INT findlen = wcslen(Find);

    while (true)
    {
        String = wcschr(String, *Find);
        if ( String == NULL)
            return(NULL);

        if (wcsncmp(String, Find, findlen) == 0)
            return((UNICHAR *) String);

        String++;
    }

    return(NULL);  // shouldn't ever hit this point.
    unguardSlow;
}


CORE_API UNICHAR* wcscat( UNICHAR* String, const UNICHAR *Add )
{
    guardSlow(wcscat);
    wcscpy(String + wcslen(String), Add);
    return(String);
    unguardSlow;
}


CORE_API UNICHAR* _wcsupr( UNICHAR* String )
{
    guardSlow(_wcsupr);
    for (UNICHAR *ptr = String; *ptr; ptr++)
        *ptr = (UNICHAR) toupper((int) *ptr);

    return(String);
    unguardSlow;
}


CORE_API INT _wtoi( const UNICHAR* Str )
{
    char *ansistr = TCHAR_TO_ANSI(Str);
    return(atoi(ansistr));
}


CORE_API INT wcstoul( const TCHAR* Start, TCHAR** End, INT Base )
{
    ANSICHAR* Ansi = TCHAR_TO_ANSI(Start);
    ANSICHAR* AnsiEnd;
    INT Result = strtoul(Ansi, &AnsiEnd, Base);
    if( End )
        *End = (TCHAR*)Start + (AnsiEnd-Ansi);
    return Result;
}

// glibc's wide functions expect a 4-byte wchar_t, so this scans ANSI copies with sscanf. It supports
// the plain %d, %f and %s conversions the importers use and widens %s results into their TCHAR buffers.
// The format follows the input string as a variadic argument to keep this apart from glibc's swscanf.
CORE_API INT swscanf( const UNICHAR* Str, ... )
{
    va_list Args;
    va_start( Args, Str );
    const UNICHAR* Fmt = va_arg( Args, const UNICHAR* );

    enum { MAX_CONVERSIONS=16, MAX_STRING=256 };
    void* Targets[MAX_CONVERSIONS] = {};
    UNICHAR* WideTargets[MAX_CONVERSIONS];
    ANSICHAR Strings[MAX_CONVERSIONS][MAX_STRING];
    ANSICHAR AnsiFmt[1024];
    INT Count=0, Len=0;
    for( const UNICHAR* F=Fmt; *F && Len<(INT)sizeof(AnsiFmt)-8; F++ )
    {
        AnsiFmt[Len++] = (ANSICHAR) *F;
        if( F[0]!='%' )
            continue;
        if( F[1]=='%' )
        {
            AnsiFmt[Len++] = (ANSICHAR) *++F;
            continue;
        }
        check(Count<MAX_CONVERSIONS);
        if( F[1]=='s' )
        {
            // Bounded, because the conversion lands in a fixed buffer first.
            Len += sprintf( AnsiFmt+Len, "%d", MAX_STRING-1 );
            WideTargets[Count] = va_arg( Args, UNICHAR* );
            Targets[Count] = Strings[Count];
        }
        else
        {
            WideTargets[Count] = NULL;
            Targets[Count] = va_arg( Args, void* );
        }
        Count++;
    }
    AnsiFmt[Len] = 0;
    va_end( Args );

    INT StrLen = appStrlen( Str );
    ANSICHAR* AnsiStr = (ANSICHAR*) appAlloca( StrLen+1 );
    for( INT i=0; i<=StrLen; i++ )
        AnsiStr[i] = (ANSICHAR) Str[i];

    INT Result = sscanf( AnsiStr, AnsiFmt, Targets[0], Targets[1], Targets[2], Targets[3], Targets[4], Targets[5],
        Targets[6], Targets[7], Targets[8], Targets[9], Targets[10], Targets[11], Targets[12], Targets[13],
        Targets[14], Targets[15] );
    for( INT i=0; i<Result && i<Count; i++ )
        if( WideTargets[i] )
            for( INT j=0; (WideTargets[i][j]=(UNICHAR)Strings[i][j])!=0; j++ );
    return Result;
}

CORE_API INT wvsnprintf( TCHAR *buf, INT max, const TCHAR *fmt, va_list args )
{
    if (fmt == NULL)
    {
        if ((max > 0) && (buf != NULL))
            *buf = 0;
        return(0);
    }
    if ((max <= 0) || (buf == NULL))
        return(0);

    TCHAR *src = (TCHAR *) appAlloca((wcslen(fmt) + 1) * sizeof (TCHAR));
    wcscpy(src, fmt);

    TCHAR *dst = buf;
    TCHAR *enddst = dst + (max - 1);

    while ((*src) && (dst < enddst))
    {
        if (*src != '%')
        {
            *dst = *src;
            dst++;
            src++;
            continue;
        }

        TCHAR *percent_ptr = src;
        INT fieldlen = -1;
        INT precisionlen = -1;

        src++; // skip the '%' char...

        while ((*src == ' ') && (dst < enddst))
        {
            *dst = ' ';
            dst++;
            src++;
        }
        if (dst >= enddst)
            break;

            // check for field width requests...
        if ((*src == '-') || ((*src >= '0') && (*src <= '9')))
        {
            TCHAR *ptr = src + 1;
            while ((*ptr >= '0') && (*ptr <= '9'))
                ptr++;

            TCHAR ch = *ptr;
            *ptr = '\0';
            fieldlen = atoi(TCHAR_TO_ANSI(src));
            *ptr = ch;

            src = ptr;
        }

        if (*src == '.')
        {
            TCHAR *ptr = src + 1;
            while ((*ptr >= '0') && (*ptr <= '9'))
                ptr++;

            TCHAR ch = *ptr;
            *ptr = '\0';
            precisionlen = atoi(TCHAR_TO_ANSI(src + 1));
            *ptr = ch;
            src = ptr;
        }

        if (*src == 'l')  /* might be meaningless; let it slide. */
            src++;

        // Longer conversions would not fit the ANSI format buffers below.
        if (src - percent_ptr > 16)
        {
            if (*src)
                src++;
            continue;
        }

        switch (*src)
        {
            case '%':
            {
                src++;
                *dst = '%';
                dst++;
            }
            break;

            case 'c':
            {
                TCHAR val = (TCHAR) va_arg(args, int);
                src++;
                *dst = val;
                dst++;
            }
            break;

            case 'd':
            case 'i':
            case 'X':
            case 'x':
            case 'u':
            {
                src++;
                int val = va_arg(args, int);
                ANSICHAR ansinum[30];
                ANSICHAR fmtbuf[30];

                // Yes, this is lame.
                INT cpyidx = 0;
                while (percent_ptr < src)
                {
                    fmtbuf[cpyidx] = (ANSICHAR) *percent_ptr;
                    percent_ptr++;
                    cpyidx++;
                }
                fmtbuf[cpyidx] = 0;

                int rc = Min<int>(snprintf(ansinum, sizeof (ansinum), fmtbuf, val), sizeof (ansinum) - 1);
                if ((dst + rc) > enddst)
                    rc = enddst - dst;
                for (int i = 0; i < rc; i++)
                {
                    *dst = (TCHAR) ansinum[i];
                    dst++;
                }
            }
            break;

            case 'I':
            {
                if ((src[1] != '6') || (src[2] != '4') || (src[3] == 0))
                {
                    debugf(TEXT("Unknown percent in UnUnix.cpp::wvsnprintf()."));
                    unicode_str_to_stdout(fmt);
                    src++;  // skip it, I guess.
                    break;
                }

                src += 4;
                QWORD val = va_arg(args, QWORD);
                ANSICHAR ansinum[60];
                ANSICHAR fmtbuf[30];
                strcpy(fmtbuf, "%L");
                percent_ptr += 4;

                // Yes, this is lame.
                INT cpyidx = 2;
                while (percent_ptr < src)
                {
                    fmtbuf[cpyidx] = (ANSICHAR) *percent_ptr;
                    percent_ptr++;
                    cpyidx++;
                }
                fmtbuf[cpyidx] = 0;

                int rc = Min<int>(snprintf(ansinum, sizeof (ansinum), fmtbuf, val), sizeof (ansinum) - 1);
                if ((dst + rc) > enddst)
                    rc = enddst - dst;
                for (int i = 0; i < rc; i++)
                {
                    *dst = (TCHAR) ansinum[i];
                    dst++;
                }
            }
            break;

            case 'f':
            {
                src++;
                double val = va_arg(args, double);
                ANSICHAR ansinum[30];
                ANSICHAR fmtbuf[30];

                // Yes, this is lame.
                INT cpyidx = 0;
                while (percent_ptr < src)
                {
                    fmtbuf[cpyidx] = (ANSICHAR) *percent_ptr;
                    percent_ptr++;
                    cpyidx++;
                }
                fmtbuf[cpyidx] = 0;

                int rc = Min<int>(snprintf(ansinum, sizeof (ansinum), fmtbuf, val), sizeof (ansinum) - 1);
                if ((dst + rc) > enddst)
                    rc = enddst - dst;
                for (int i = 0; i < rc; i++)
                {
                    *dst = (TCHAR) ansinum[i];
                    dst++;
                }
            }
            break;

            case 's':
            {
                src++;
                TCHAR *val = va_arg(args, TCHAR *);
                if (val == NULL)
                    val = TEXT("(null)");

                int rc = wcslen(val);
                if ((dst + rc) > enddst)
                    rc = enddst - dst;
                for (int i = 0; i < rc; i++)
                {
                    *dst = *val;
                    dst++;
                    val++;
                }
            }
            break;

            default:
                if (*src)
                    src++;  // skip char, I guess.
                debugf(TEXT("Unknown percent in UnUnix.cpp::wvsnprintf()."));
                unicode_str_to_stdout(fmt);
                break;
        }
    }

    *dst = 0;  // null terminate the new string.
    return(dst - buf);
}


CORE_API INT wprintf( const TCHAR* fmt, ... )
{
    va_list args;
    TCHAR buf[1024];  // (*shrug*)

    va_start(args, fmt);
    INT rc = wvsnprintf(buf, sizeof (buf) / sizeof (TCHAR), fmt, args);
    va_end(args);

    if (GUnicodeOS)
        fwrite(buf, sizeof (TCHAR), rc, stdout);
    else
        fwrite(appToAnsi(buf), sizeof (ANSICHAR), rc, stdout);

    return(rc);
}

CORE_API INT iswspace( UNICHAR ch  )
{
    return((INT) isspace((int) ch));
}

CORE_API TCHAR *_itow( const INT Num, TCHAR *Buffer, const INT Radix )
{
    return(_i64tow((SQWORD) Num, Buffer, Radix));
}

CORE_API QWORD _wcstoui64( const TCHAR* Start, TCHAR** End, INT Base )
{
    check((Base >= 2) && (Base <= 36));

    while (*Start == ' ')
        Start++;

    const TCHAR *ptr = Start;
    while (1)
    {
        char ch = toupper((char) *ptr);

        if ((ch >= 'A') && (ch <= 'Z'))
        {
            if ((Base <= 10) || (ch >= ('A' + Base)))
                break;
        }

        else if ((ch >= '0') && (ch <= '9'))
        {
            if (ch >= ('0' + Base))
                break;
        }

        else
            break;

        ptr++;
    }

    if (End != NULL)
        *End = const_cast<TCHAR *>(ptr);

    QWORD retval = 0;
    QWORD accumulator = 1;
    while (--ptr >= Start)
    {
        char ch = toupper((char) *ptr);
        QWORD val = (((ch >= 'A') && (ch <= 'Z')) ? (ch - 'A') + 10 : ch - '0');
        retval += val * accumulator;
        accumulator *= (QWORD) Base;
    }

    return(retval);
}


CORE_API TCHAR* _ui64tow( QWORD Num, TCHAR *Buffer, INT Base )
{
    check((Base >= 2) && (Base <= 36));

    int bufidx = 1;
    for (QWORD x = Num; x >= Base; x /= Base)
        bufidx++;

    Buffer[bufidx] = 0;  /* null terminate where the end will be. */

    while (--bufidx >= 0)
    {
        int val = (int) (Num % Base);
        Buffer[bufidx] = ((val > 9) ? ('a' + (val - 10)) : ('0' + val));
        Num /= Base;
    }

    return(Buffer);
}

CORE_API TCHAR* _i64tow( SQWORD Num, TCHAR *Buffer, INT Base )
{
    check((Base >= 2) && (Base <= 36));

    int bufidx = 1;
    int negative = 0;
    if (Num < 0)
    {
        negative = 1;
        Num = -Num;
        bufidx++;
        Buffer[0] = '-';
    }

    for (QWORD x = Num; x >= Base; x /= Base)
        bufidx++;

    Buffer[bufidx] = 0;  /* null terminate where the end will be. */

    while ((--bufidx - negative) >= 0)
    {
        int val = (int) (Num % Base);
        Buffer[bufidx] = ((val > 9) ? ('a' + (val - 10)) : ('0' + val));
        Num /= Base;
    }

    return(Buffer);
}


CORE_API FLOAT _wtof( const TCHAR* Str )
{
    return((FLOAT) atof(appToAnsi(Str)));
}

#endif


/* Visual C++ compatibility. */
char *strlwr(char *str)
{
    for (char *p = str; *p; p++)
        *p = (char) tolower((int) *p);

    return(str);
}


CORE_API const UBOOL appMsgf( INT Type, const TCHAR* Fmt, ... )
{
	TCHAR TempStr[4096]=TEXT("");

	GET_VARARGS( TempStr, ARRAY_COUNT(TempStr), Fmt, Fmt );

    debugf( NAME_Warning, TEXT("appMsgf(): %s"), TempStr );

    return 1;
}



// Interface for recording loading errors in the editor
CORE_API const void EdClearLoadErrors()
{
	GEdLoadErrors.Empty();
}


/*-----------------------------------------------------------------------------
	USystem.
-----------------------------------------------------------------------------*/

//
// System manager.
//
static void Recurse()
{
	guard(Recurse);
	Recurse();
	unguard;
}
USystem::USystem()
:	SavePath	( E_NoInit )
,	CachePath	( E_NoInit )
,	CacheExt	( E_NoInit )
,	Paths		( E_NoInit )
,	Suppress	( E_NoInit )
,   CacheRecordPath( E_NoInit )
,	MusicPath	( E_NoInit )
,	SpeechPath	( E_NoInit )
// amb ---
,   LicenseeMode (1) 
// --- amb
{}
void USystem::StaticConstructor()
{
	guard(USystem::StaticConstructor);

	new(GetClass(),TEXT("PurgeCacheDays"),      RF_Public)UIntProperty   (CPP_PROPERTY(PurgeCacheDays     ), TEXT("Options"), CPF_Config );
	new(GetClass(),TEXT("SavePath"),            RF_Public)UStrProperty   (CPP_PROPERTY(SavePath           ), TEXT("Options"), CPF_Config );
	new(GetClass(),TEXT("CacheExt"),            RF_Public)UStrProperty   (CPP_PROPERTY(CacheExt           ), TEXT("Options"), CPF_Config );
	new(GetClass(),TEXT("SpeechPath"),          RF_Public)UStrProperty   (CPP_PROPERTY(SpeechPath         ), TEXT("Options"), CPF_Config );

	UArrayProperty* A = new(GetClass(),TEXT("Paths"),RF_Public)UArrayProperty( CPP_PROPERTY(Paths), TEXT("Options"), CPF_Config );
	A->Inner = new(A,TEXT("StrProperty0"),RF_Public)UStrProperty;

	UArrayProperty* B = new(GetClass(),TEXT("Suppress"),RF_Public)UArrayProperty( CPP_PROPERTY(Suppress), TEXT("Options"), CPF_Config );
	B->Inner = new(B,TEXT("NameProperty0"),RF_Public)UNameProperty;

	UArrayProperty* C = new(GetClass(),TEXT("CacheRecordPath"),RF_Public)UArrayProperty( CPP_PROPERTY(CacheRecordPath), TEXT("Options"), CPF_Config );
	C->Inner = new(C,TEXT("StrProperty1"),RF_Public)UStrProperty;

	UArrayProperty* D = new(GetClass(),TEXT("MusicPath"),RF_Public)UArrayProperty( CPP_PROPERTY(MusicPath), TEXT("Options"), CPF_Config );
	D->Inner = new(D,TEXT("StrProperty2"),RF_Public)UStrProperty;

	unguard;
}

static bool FindBinaryInPath(FString &cmd);

UBOOL USystem::Exec( const TCHAR* Cmd, FOutputDevice& Ar )
{
	if( ParseCommand(&Cmd,TEXT("MEMSTAT")) )
	{
		//!UNIX No MEMSTAT command.
		Ar.Logf( TEXT("MEMSTAT command not available.") );
		return 1;
	}
	else if( ParseCommand(&Cmd,TEXT("EXIT")) )
	{
		Ar.Log( TEXT("Closing by request") );
		appRequestExit( 0 );
		return 1;
	}
	else if( ParseCommand(&Cmd,TEXT("APP")) )
	{
		//!UNIX No APP command.
		Ar.Logf( TEXT("APP command not available.") );
		return 1;
	}
	else if( ParseCommand( &Cmd, TEXT("RELAUNCH") ) )
	{
		debugf( TEXT("Relaunch: %s"), Cmd );
		GConfig->Flush( 0 );

#if __IOS__
		// An iOS app can't start another process, and it is suspended in the background, so it can't host a dedicated server.
		Ar.Logf( TEXT("A dedicated server can't run on iOS, host a listen server instead.") );
		return 1;
#elif __ANDROID__ || __EMSCRIPTEN__
		// Neither an app nor a page can start a copy of itself. SDLLaunch passes the arguments to its server service, or to the reloaded page.
		if( !ParseParam( Cmd, TEXT("SERVER") ) )
		{
			Ar.Logf( TEXT("Only a dedicated server can be relaunched here.") );
			return 1;
		}
		GUnixSpawnOnExit = new TCHAR[appStrlen(Cmd) + 1];
		appStrcpy( GUnixSpawnOnExit, Cmd );
#else
        if ((GIsClient) && (appStrlen(Cmd) == 0))  // just restarting client?
        {
            GUnixSpawnOnExit = new TCHAR[128];
            appStrcpy(GUnixSpawnOnExit, TEXT("./ut2004-bin"));
            appRequestExit( 0 );
            return 1;
        }

		// Fork out a new process using Cmd as arguments.
		TCHAR* EndArg0 = appStrstr( Cmd, TEXT(" ") );
		INT CmdLen = appStrlen( Cmd );
		INT RestLen = ((EndArg0 != NULL) ? appStrlen( EndArg0 ) : 0);
        #define RELAUNCH_ARRAY 1024
		TCHAR Arg0[RELAUNCH_ARRAY];
        INT max = (CmdLen - RestLen) + 1;
        if (max > RELAUNCH_ARRAY)
            max = RELAUNCH_ARRAY;
		appStrncpy( Arg0, Cmd, max );
        Arg0[RELAUNCH_ARRAY - 1] = 0;  // just in case.

        INT arraylen = RELAUNCH_ARRAY + 128;
        FString xterm(TEXT(""));

            xterm = TEXT("xterm");
            if (FindBinaryInPath(xterm))
            {
                arraylen += xterm.Len();
                xterm += TEXT(" -e ");
            }
            else
            {
                xterm = TEXT("");
            }

            if (xterm.Len())
            {
                GUnixSpawnOnExit = new TCHAR[arraylen];
                appStrcpy(GUnixSpawnOnExit, *xterm);
                appStrcat(GUnixSpawnOnExit, TEXT("./ucc-bin server "));
                appStrcat(GUnixSpawnOnExit, Arg0);
            }
            else
            {
                // Without a terminal ucc-bin would run unseen, the launcher shows a tray icon for its server.
                GUnixSpawnOnExit = new TCHAR[CmdLen + 32];
                appStrcpy(GUnixSpawnOnExit, TEXT("./ut2004-bin "));
                appStrcat(GUnixSpawnOnExit, Cmd);
            }
#endif

        appRequestExit( 0 );
		return 1;
	}
	else if( ParseCommand( &Cmd, TEXT("DEBUG") ) )
	{
		if( ParseCommand(&Cmd,TEXT("CRASH")) )
		{
			appErrorf( TEXT("%s"), TEXT("Unreal crashed at your request") );
			return 1;
		}
		else if( ParseCommand( &Cmd, TEXT("GPF") ) )
		{
			Ar.Log( TEXT("Unreal crashing with voluntary GPF") );
			*(int *)NULL = 123;
			return 1;
		}
		else if( ParseCommand( &Cmd, TEXT("RECURSE") ) )
		{
			Ar.Logf( TEXT("Recursing") );
			Recurse();
			return 1;
		}
		else if( ParseCommand( &Cmd, TEXT("EATMEM") ) )
		{
			Ar.Log( TEXT("Eating up all available memory") );
			while( 1 )
			{
				void* Eat = appMalloc(65536,TEXT("EatMem"));
				memset( Eat, 0, 65536 );
			}
			return 1;
		}
		else return 0;
	}
	else return 0;
}
IMPLEMENT_CLASS(USystem);

/*-----------------------------------------------------------------------------
	Exit.
-----------------------------------------------------------------------------*/

//
// Immediate exit.
//
CORE_API void appRequestExit( UBOOL Force )
{
	guard(appForceExit);
	debugf( TEXT("appRequestExit(%i)"), Force );
	if( Force )
	{
		// Force immediate exit. Dangerous because config code isn't flushed, etc.
		exit( 1 );
	}
	else
	{
		// Tell the platform specific code we want to exit cleanly from the main loop.
		//!UNIX No quit message in UNIX.
		GIsRequestingExit = 1;
	}
	unguard;
}

/*-----------------------------------------------------------------------------
	Shared libraries.
-----------------------------------------------------------------------------*/

#if __EMSCRIPTEN__ || __IOS__
// Generated by the build from the list of linked modules.
extern "C" void* appLinkedPackage( const char* Symbol );
#endif

#if __IOS__
// SDL is linked into the app, Core only needs this one function of it.
extern "C" bool SDL_OpenURL( const char* Url );
#endif

//
// Load a library.
//
void* appGetDllHandle( const TCHAR* Filename )
{
	guard(appGetDllHandle);

	void *Result = NULL;

	check(Filename);
	const TCHAR* PackageName = Filename;
	TCHAR* Cur;
	TCHAR* Test = appStaticString1024();

	// Get GLoadedPackage symbol name from full path.
 	while( (Cur = appStrchr( PackageName, '/' )) != NULL )
		PackageName = Cur + 1;
 	while( (Cur = appStrchr( PackageName, '\\' )) != NULL )
		PackageName = Cur + 1;
	appSprintf( Test, TEXT("GLoaded%s"), PackageName );
	if( (Cur = appStrchr( Test, '.' )) != NULL )
		*Cur = '\0';

#if __EMSCRIPTEN__ || __IOS__
	// dlsym needs a MAIN_MODULE build, and nothing can be loaded at runtime anyway.
	// iOS apps can't load libraries of their own either, and the linker drops symbols nothing references.
	return appLinkedPackage( appToAnsi(Test) );
#endif

	const char* Error;
	dlerror();	// Clear any error condition.

	// Check if the library was linked to the executable.
#if __ANDROID__
	// The engine is libmain.so, loaded RTLD_LOCAL by the Java side, so the global scope doesn't hold its symbols.
	Dl_info Self;
	dladdr( (void*)&appGetDllHandle, &Self );
	Result = (void*)dlopen( Self.dli_fname, RTLD_NOW | RTLD_NOLOAD );
#else
	Result = (void*)dlopen( NULL, RTLD_NOW | RTLD_GLOBAL );
#endif
	Error = dlerror();
	if( Error != NULL )
		debugf( TEXT("dlerror(): %s"), Error );
	else
	{
		// Only dlerror's verdict on the lookup is needed.
		(void)dlsym( Result, appToAnsi(Test) ); // NOLINT(bugprone-unused-return-value)
		Error = dlerror();
		if( Error == NULL )
			return Result;
	}

	// Load the new library.
	Result = (void*)dlopen( appToAnsi(Filename), RTLD_NOW | RTLD_GLOBAL );
	if( Result == NULL )
	{
		TCHAR Temp[256];
		appStrcpy( Temp, Filename );
		appStrcat( Temp, DLLEXT );
		Result = (void*)dlopen( appToAnsi(Temp), RTLD_NOW | RTLD_GLOBAL );
        #ifdef LOOKING_FOR_UNDEFINED_SYMBOLS
    		Error = dlerror();
    		if( Error != NULL )
    		debugf( "dlerror(): %s", Error );
        #endif
	}

	if (Result != NULL)
	{
		const char *envr = getenv("UNREAL_PAUSE_ON_DLOPEN");
		if (envr)
		{
			const TCHAR *libname = ANSI_TO_TCHAR(envr);
			if (appStrfind(Filename, libname))
			{
				printf("%s loaded. Sleeping ten seconds...\n", envr);
				sleep(10);
			}
		}
	}

	return Result;
	unguard;
}

//
// Free a library.
//
void appFreeDllHandle( void* DllHandle )
{
	guard(appFreeDllHandle);
	check(DllHandle);

	if (DllHandle == (void *) -1)
		return;

#if !__EMSCRIPTEN__ && !__IOS__
	dlclose( DllHandle );
#endif

	unguard;
}

//
// Lookup the address of a shared library function.
//
void* appGetDllExport( void* DllHandle, const TCHAR* ProcName )
{
	guard(appGetDllExport);
	check(DllHandle);
	check(ProcName);

	void* Result = NULL;

	const char *Error;

	dlerror();	// Clear any error condition.

	Result = (void*)dlsym( DllHandle, appToAnsi(ProcName) );
	Error = dlerror();
	if( Error != NULL )
		debugf( TEXT("dlerror: %s"), ANSI_TO_TCHAR(Error) );

	return Result;

	unguard;
}

//
// Break the debugger.
//
void appDebugBreak()
{
	guard(appDebugBreak);
	raise(SIGTRAP);
	unguard;
}

//
// IsDebuggerPresent()
//
UBOOL appIsDebuggerPresent()
{
	// Does not need to be implemented.
	return 0;
}

/*-----------------------------------------------------------------------------
	External processes.
-----------------------------------------------------------------------------*/

static TMap<pid_t,int>* ExitCodeMap = NULL;

void* appCreateProc( const TCHAR* URL, const TCHAR* Parms )
{
	guard(appCreateProc);

	debugf( TEXT("Create Proc: %s %s"), URL, Parms );

	TCHAR LocalParms[PATH_MAX] = TEXT("");
	appStrcpy(LocalParms, URL);
	appStrcat(LocalParms, TEXT(" "));
	appStrcat(LocalParms, Parms);

#if __IOS__
	// iOS has no system(), and apps can't start processes.
	return NULL;
#else
	pid_t pid = fork();
	if (pid == 0)
		_exit( system( appToAnsi(LocalParms) ) );

	return (void*) ((PTRINT) pid);
#endif
	unguard;
}

UBOOL appGetProcReturnCode( void* ProcHandle, INT* ReturnCode )
{
	guard(appGetProcReturnCode);
	int* p = ExitCodeMap->Find( (pid_t) ((PTRINT) ProcHandle) );
	if(p)
	{
		*ReturnCode = *p;
		ExitCodeMap->Remove( (pid_t) ((PTRINT) ProcHandle) );
		return 1;
	}
	return 0;
	unguard;
}

/*-----------------------------------------------------------------------------
	Signal Handling.
-----------------------------------------------------------------------------*/

static void HandleChild(int Signal)
{
	int Status;
	pid_t pid;
	while( (pid=waitpid(-1, &Status, WNOHANG)) > 0 )
		ExitCodeMap->Set( pid, WEXITSTATUS(Status) );
}

static pid_t mainPID;
static INT AlreadyAborting	= 0;
static INT SignalExit		= 0;
static INT SignalCritical	= 0;

static void HandleSignal( int Signal )
{
	char* sig_text;
	switch (Signal)
	{
		case SIGHUP:
			sig_text = "Signal: SIGHUP [hangup]";
			SignalExit++;
			break;
		case SIGQUIT:
			sig_text = "Signal: SIGQUIT [quit]";
			SignalExit++;
			break;
		case SIGILL:
			sig_text = "Signal: SIGILL [illegal instruction]";
			SignalCritical++;
			break;
		case SIGTRAP:
			sig_text = "Signal: SIGTRAP [trap]";
			SignalCritical++;
			break;
		case SIGIOT:
			sig_text = "Signal: SIGIOT [iot trap]";
			SignalCritical++;
			break;
		case SIGBUS:
			sig_text = "Signal: SIGBUS [bus error]";
			SignalCritical++;
			break;
		case SIGFPE:
			sig_text = "Signal: SIGFPE [floating point exception]";
			SignalCritical++;
			break;
		case SIGSEGV:
			sig_text = "Signal: SIGSEGV [segmentation fault]";
			SignalCritical++;
			break;
		case SIGTERM:
			sig_text = "Signal: SIGTERM [terminate]";
			SignalExit++;
			break;
		case SIGINT:  // must have gone through HandleInterrupt previously.
			sig_text = "Signal: SIGINT [interrupt]";
			SignalCritical++;
			break;
		default:
			sig_text = "Signal: UNKNOWN SIGNAL [%i]";
			SignalCritical++;
			break;
	}

	if ( (SignalCritical > 0) || (SignalExit > 1) )
	{
		if (!AlreadyAborting)
		{
			// Avoid calling appExit again.
			AlreadyAborting = true;

			printf( "%s\n", sig_text );
			printf( "Aborting.\n\n" );
			printf( "\nCrash information will be saved to your logfile.\n" );

			#if !__ANDROID__ && !__EMSCRIPTEN__
				void *aTrace[64];
				char **tString;
				int size = backtrace(aTrace, ARRAY_COUNT(aTrace));
				tString = backtrace_symbols(aTrace, size);
				debugf(TEXT("\nDeveloper Backtrace:"));
				for (int i = 0; i < size; i++)
					debugf(TEXT("[%2.i]  %s"), i+1, appFromAnsi(tString[i]));
			#endif

			#if DO_PORTABLE_GUARD_BLOCKS
				FString GuardBackTrace;
				UnGuardBlock::GetBackTrace(GuardBackTrace);
				debugf(TEXT("Unreal Call Stack: %s"), *GuardBackTrace);
			#endif

			appExit();
		}
		exit(1);
	}
	if ( SignalExit == 1 )
	{
		printf("%s\n", sig_text);
		printf("Requesting Exit.\n");
		appRequestExit( 0 );
	}
}


// Special case for CTRL-C.
static void HandleInterrupt( int Signal )
{
    check(Signal==SIGINT);

    if (getpid() == mainPID)
    {
        static bool alreadyTriggered = false;

        if (alreadyTriggered)
        {
            // User is mashing CTRL-C, or appRequestExit() work is deadlocked.
            // Pass SIGINT to our standard interrupt handler for panic crash.
            HandleSignal(Signal);
        }

        alreadyTriggered = true;

        if (GIsRunning)   // already in main loop.
        {
            // let the engine try to shut down cleanly.
            debugf(TEXT("CTRL-C has been pressed.\n"));
            appRequestExit(0);
        }

        else
        {
            // Need to force an immediate termination with _exit(),
            //  since things with static destructors may not be initialized
            //  yet! Otherwise, this could cause a confusing segfault in
            //  the normal exit() processing. Please note that calling
            //  appRequestExit(1) will result in a normal exit() call.
            printf("CTRL-C before main loop ... forcing exit.\n");
            _exit(42);  // all threads should die.
        }
    }
}


/*-----------------------------------------------------------------------------
	Timing.
-----------------------------------------------------------------------------*/

//
// String timestamp.
//
CORE_API const TCHAR* appTimestamp()
{
	guard(appTimestamp);

	TCHAR* 		Result = appStaticString1024();
	time_t		CurTime;
	struct tm*	SysTime;

	CurTime = time( NULL );
	SysTime = localtime( &CurTime );
	ANSICHAR Text[64];
	strftime( Text, sizeof(Text), "%a %b %e %H:%M:%S %Y", SysTime );
	appStrcpy( Result, ANSI_TO_TCHAR(Text) );
	return Result;

	unguard;
}

#if !DEFINED_appCycles
// Microseconds, GSecondsPerCycle is set to match in appPlatformInit.
CORE_API DWORD appCycles()
{
	return (DWORD)(QWORD)(appSeconds() * 1000000.0);
}
#endif


//
// Get time in seconds. Origin is arbitrary.
//
#if !DEFINED_appSeconds
CORE_API DOUBLE appSeconds()
{
	static timespec Start;
	timespec Now;
	clock_gettime( CLOCK_MONOTONIC, &Now );
	if( !Start.tv_sec && !Start.tv_nsec )
		Start = Now;
	return (Now.tv_sec - Start.tv_sec) + (Now.tv_nsec - Start.tv_nsec) / 1000000000.0;
}
#endif




//
// Return the system time.
//
CORE_API void appSystemTime( INT& Year, INT& Month, INT& DayOfWeek, INT& Day, INT& Hour, INT& Min, INT& Sec, INT& MSec )
{
	guard(appSystemTime);

	time_t			CurTime;
	struct tm		*St;		// System time.
	struct timeval	Tv;			// Use timeval to get milliseconds.

	gettimeofday( &Tv, NULL );
	CurTime = time( NULL );
	St = localtime( &CurTime );

	Year		= St->tm_year + 1900;
	Month		= St->tm_mon + 1;
	DayOfWeek	= St->tm_wday;
	Day			= St->tm_mday;
	Hour		= St->tm_hour;
	Min			= St->tm_min;
	Sec			= St->tm_sec;
	MSec		= (INT) (Tv.tv_usec / 1000);

	unguard;
}

CORE_API void appSleep( FLOAT Seconds )
{
	guard(appSleep);

	INT SleepTime = (INT) (Seconds * 1000000);
	usleep( SleepTime );

	unguard;
}

/*-----------------------------------------------------------------------------
	Link functions.
-----------------------------------------------------------------------------*/

static bool FindBinaryInPath(FString &cmd)
{
    INT binpos = cmd.InStr(TEXT(" "));
    FString bin((binpos == -1) ? cmd : cmd.Left(binpos));

    if (bin.Len() == 0)
        return(false);

    if (bin.InStr(TEXT("/")) != -1)  // path specified; don't examine $PATH.
        return(access(appToAnsi(*bin), X_OK) != -1);

    const char *_envr = getenv("PATH");
    if (_envr == NULL)
    {
        debugf(TEXT("No $PATH environment var set"));
        return(false);
    }

    FString envr(_envr);

    while (true)
    {
        INT pos = envr.InStr(TEXT(":"));
        FString path((pos == -1) ? envr : envr.Left(pos));
        path += TEXT("/");

        FString binpath = path;
        binpath += bin;

        char realized[MAXPATHLEN];
        if (realpath(appToAnsi(*binpath), realized) != NULL)
        {
            struct stat statbuf;
            if (stat(realized, &statbuf) != -1)
            {
                /* dirs are usually "executable", but aren't programs. */
                if (!S_ISDIR(statbuf.st_mode))
                {
                    if (access(realized, X_OK) != -1)
                    {
                        cmd = FString(realized) + cmd.Mid(binpos);
                        return(true);
                    }
                }
            }
        }

        if (pos == -1)
            break;
        envr = envr.Mid(pos + 1);
    }

    return(false);
}


//
// Launch a uniform resource locator (i.e. http://www.epicgames.com/unreal).
// This is expected to return immediately as the URL is launched by another
// task.
//
void appLaunchURL( const TCHAR* URL, const TCHAR* Parms, FString* Error )
{
	guard(appLaunchURL);

#if __IOS__
	// Statically linked into the app. UIKit opens the page in the browser.
	UBOOL Failed = !SDL_OpenURL( appToAnsi(URL) );
	if( Error )
		*Error = Failed ? LocalizeError(TEXT("UrlFailed"),TEXT("Core")) : TEXT("");
#elif __ANDROID__
	// There is no xdg-open, and the spawn API below needs API 34.
	if( Error )
		*Error = LocalizeError(TEXT("UrlFailed"),TEXT("Core"));
#elif __EMSCRIPTEN__
	EM_ASM( window.open(UTF8ToString($0), '_blank', 'noopener'), appToAnsi(URL) );
	if( Error )
		*Error = TEXT("");
#else
	// HandleChild reaps the process.
	char Url[PATH_MAX];
	appToAnsi( URL, Url, sizeof(Url) );
	char Opener[] = "xdg-open";
	char* Argv[] = { Opener, Url, NULL };
	// The browser outlives the game, it must not keep its sockets, log and audio device open.
	posix_spawn_file_actions_t Actions;
	posix_spawn_file_actions_init( &Actions );
	posix_spawn_file_actions_addclosefrom_np( &Actions, 3 );
	pid_t Pid;
	UBOOL Failed = posix_spawnp( &Pid, Opener, &Actions, NULL, Argv, environ ) != 0;
	posix_spawn_file_actions_destroy( &Actions );
	if( Failed )
		debugf( NAME_Warning, TEXT("Couldn't run xdg-open for %s"), URL );
	if( Error )
		*Error = Failed ? LocalizeError(TEXT("UrlFailed"),TEXT("Core")) : TEXT("");
#endif

	unguard;
}

/*-----------------------------------------------------------------------------
	File finding.
-----------------------------------------------------------------------------*/

//
// Clean out the file cache.
//
static INT GetFileAgeDays( const TCHAR* Filename )
{
	guard(GetFileAgeDays);
	struct stat Buf;
	INT Result = 0;

	Result = stat(TCHAR_TO_ANSI(Filename),&Buf);
	if( Result==0 )
	{
		time_t CurrentTime, FileTime;
		FileTime = Buf.st_mtime;
		time( &CurrentTime );
		DOUBLE DiffSeconds = difftime( CurrentTime, FileTime );
		return (INT)(DiffSeconds / 60.0 / 60.0 / 24.0);
	}
	return 0;
	unguard;
}

CORE_API void appCleanFileCache()
{
	guard(appCleanFileCache);

	// Delete all temporary files.
	guard(DeleteTemps);
	FString Temp = FString::Printf( TEXT("%s%s*.tmp"), *GSys->CachePath, PATH_SEPARATOR );
	TArray<FString> Found = GFileManager->FindFiles( *Temp, 1, 0 );
	for( INT i=0; i<Found.Num(); i++ )
	{
		Temp = FString::Printf( TEXT("%s%s%s"), *GSys->CachePath, PATH_SEPARATOR, *Found(i) );
		debugf( TEXT("Deleting temporary file: %s"), *Temp );
		GFileManager->Delete( *Temp );
	}
	unguard;

	// Delete cache files that are no longer wanted.
	guard(DeleteExpired);
	TArray<FString> Found = GFileManager->FindFiles( *(GSys->CachePath * TEXT("*") + GSys->CacheExt), 1, 0 );
	if( GSys->PurgeCacheDays )
	{
		for( INT i=0; i<Found.Num(); i++ )
		{
			FString Temp = FString::Printf( TEXT("%s%s%s"), *GSys->CachePath, PATH_SEPARATOR, *Found(i) );
			INT DiffDays = GetFileAgeDays( *Temp );
			if( DiffDays > GSys->PurgeCacheDays )
			{
				debugf( TEXT("Purging outdated file from cache: %s (%i days old)"), *Temp, DiffDays );
				GFileManager->Delete( *Temp );
			}
		}
	}
	unguard;

	unguard;
}

/*-----------------------------------------------------------------------------
	Guids.
-----------------------------------------------------------------------------*/

//
// Create a new globally unique identifier.
//
CORE_API FGuid appCreateGuid()
{
	guard(appCreateGuid);

	// Random like CoCreateGuid on Windows, callers use it for secrets too.
	FGuid Result;
#if __APPLE__
	// The iOS SDK doesn't declare getentropy. This takes the same system entropy and cannot fail.
	arc4random_buf( &Result, sizeof(Result) );
#else
	verify( getentropy( &Result, sizeof(Result) )==0 );
#endif
	return Result;

	unguard;
}

/*-----------------------------------------------------------------------------
	Command line.
-----------------------------------------------------------------------------*/

// Get startup directory.
CORE_API const TCHAR* appBaseDir()
{
	guard(appBaseDir);
	static TCHAR BaseDir[PATH_MAX]=TEXT("");
    ANSICHAR AnsiBaseDir[PATH_MAX];

	if( !BaseDir[0] )
	{
		// If the executable isn't launched from its own directory, then the
		// dynamic linker won't be able to find the shared libraries.
		getcwd( AnsiBaseDir, sizeof(AnsiBaseDir) );
        appStrcpy( BaseDir, ANSI_TO_TCHAR(AnsiBaseDir));
		appStrcat( BaseDir, TEXT("/") );
	}
	return BaseDir;
	unguard;
}

// Get computer name.
CORE_API const TCHAR* appComputerName()
{
	guard(appComputerName);
	static TCHAR Result[256]=TEXT("");
	static ANSICHAR AnsiResult[256];
	if( !Result[0] )
	{
		gethostname( AnsiResult, sizeof(AnsiResult) );
		appStrcpy( Result, ANSI_TO_TCHAR(AnsiResult));
	}
	return Result;
	unguard;
}

// Get user name.
CORE_API const TCHAR* appUserName()
{
	guard(appUserName);
	static TCHAR Result[256]=TEXT("");
	if( !Result[0] )
	{
		ANSICHAR *str = getlogin();
		appStrncpy( Result, ANSI_TO_TCHAR(str), sizeof(Result) );
	}
	return Result;
	unguard;
}

// Get launch package base name.
CORE_API const TCHAR* appPackage()
{
	guard(appPackage);
	return(appStrcpy(appStaticString1024(), ANSI_TO_TCHAR(GModule)));
	unguard;
}



/*-----------------------------------------------------------------------------
	App init/exit.
-----------------------------------------------------------------------------*/

//
// Platform specific initialization.
//
void appPlatformPreInit()
{
#if UNICODE
    unixDetectUNICODE();
#endif

#if __LINUX_X86__
    #if __ANDROID__
        // No OS enum exists for it, and the 64-bit Linux one is the closest to what servers expect.
        GRunningOS = OS_LINUX_X86_64;
        appSprintf(GMachineOS, TEXT("arm64 Android"));
    #elif __IOS__
        GRunningOS = OS_LINUX_X86_64;
        appSprintf(GMachineOS, TEXT("arm64 iOS"));
    #elif __EMSCRIPTEN__
        GRunningOS = OS_LINUX_X86_64;
        appSprintf(GMachineOS, TEXT("Web"));
    #elif defined __aarch64__
        GRunningOS = OS_LINUX_X86_64;
        appSprintf(GMachineOS, TEXT("arm64 Linux"));
    #elif defined __x86_64__
        GRunningOS = OS_LINUX_X86_64;
        appSprintf(GMachineOS, TEXT("x86-64 Linux"));
    #else
        GRunningOS = OS_LINUX_X86;
        appSprintf(GMachineOS, TEXT("x86 Linux"));
    #endif

#else
    #error fill in your platform.
#endif

    mainPID = getpid();
}


typedef struct
{
    pthread_mutex_t mutex;
    pthread_t tid;
    INT refcount;
} pthread_mutex_block;

FCriticalSection::FCriticalSection()
{
	guard(FCriticalSection::FCriticalSection);
	pthread_mutex_block *block = new pthread_mutex_block;
	int rc = pthread_mutex_init(&block->mutex, NULL);
	check(rc == 0);
    block->refcount = 0;
    block->tid = 0;
    Handle = block;
	unguard;
}

FCriticalSection::~FCriticalSection() noexcept(false)
{
	guard(FCriticalSection::~FCriticalSection);
	pthread_mutex_block *block = (pthread_mutex_block *) Handle;
	int rc = pthread_mutex_destroy(&block->mutex);
	check(rc == 0);
    delete block;
	unguard;
}

void FCriticalSection::Lock()
{
	pthread_mutex_block *block = (pthread_mutex_block *) Handle;
    pthread_t self = pthread_self();
    if (self == block->tid)
    {
        block->refcount++;
    }
    else
    {
	    int rc = pthread_mutex_lock(&block->mutex);
	    check(rc == 0);
        check(block->refcount == 0);
        block->tid = self;
        block->refcount = 1;
    }
}

void FCriticalSection::Unlock()
{
	pthread_mutex_block *block = (pthread_mutex_block *) Handle;
    pthread_t self = pthread_self();
    check(self == block->tid);
    block->refcount--;
    check(block->refcount >= 0);
    if (block->refcount == 0)
	{
        block->tid = 0;
        int rc = pthread_mutex_unlock(&block->mutex);
	    check(rc == 0);
    }
}

FScopeCriticalSection::FScopeCriticalSection( FCriticalSection* InCriticalSection )
{
	CriticalSection = InCriticalSection;
	CriticalSection->Lock();
}

FScopeCriticalSection::~FScopeCriticalSection()
{
	CriticalSection->Unlock();
}


#define DESIRED_FILE_HANDLES 1024
void appPlatformInit()
{
	guard(appPlatformInit);

	// System initialization.
	GSys = new USystem;
	GSys->AddToRoot();
	const char* XdgCache = getenv("XDG_CACHE_HOME");
	if( XdgCache && XdgCache[0]=='/' )
		GSys->CachePath = FString(appFromAnsi(XdgCache)) * TEXT("ut2004");
	else
	{
		const char* Home = getenv("HOME");
		if( !Home || Home[0]!='/' )
		{
			struct passwd* Pwd = getpwuid( getuid() );
			Home = Pwd ? Pwd->pw_dir : "/tmp";
		}
		GSys->CachePath = FString(appFromAnsi(Home)) * TEXT(".cache") * TEXT("ut2004");
	}
	// Old mods such as AntiTCC 1.19 locate package files by the ".." that starts a retail relative path.
	// Relative to the file manager's base dir, because that is where it resolves writes.
	{
		FString BaseStr = GFileManager->CalcHomeDir();
		if( BaseStr.Right(1) != TEXT("/") )
			BaseStr += TEXT("/");
		const TCHAR* Base = *BaseStr;
		FString Target = GSys->CachePath + TEXT("/");
		INT Common = 0;
		for( INT i=0; Base[i] && Base[i]==(*Target)[i]; i++ )
			if( Base[i]=='/' )
				Common = i+1;
		FString Relative;
		for( INT i=Common; Base[i]; i++ )
			if( Base[i]=='/' )
				Relative += TEXT("../");
		Relative += Target.Mid( Common );
		if( Relative.Len() )
			GSys->CachePath = Relative.LeftChop( 1 );
	}
	for( INT i=0; i<GSys->Suppress.Num(); i++ )
		GSys->Suppress(i).SetFlags( RF_Suppress );

	// Randomize.
	srand( (unsigned)time( NULL ) );

	// Exit code handling
	ExitCodeMap = new TMap<pid_t,int>;

	debugf( NAME_Init, TEXT("OS Type=%s"), GMachineOS );

	GSecondsPerCycle = 0.000001;

	struct sigaction SigAction;

	// Only try once.
	INT DefaultFlag			= SA_RESETHAND;

	SigAction.sa_handler	= HandleSignal;
	SigAction.sa_flags		= DefaultFlag;
	sigemptyset( &SigAction.sa_mask );
	sigaction( SIGSEGV, &SigAction, 0 );

	SigAction.sa_handler	= HandleSignal;
	SigAction.sa_flags		= DefaultFlag;
	sigemptyset( &SigAction.sa_mask );
	sigaction( SIGILL, &SigAction, 0 );

	// Try multiple times.
	DefaultFlag				= 0;

	SigAction.sa_handler	= HandleChild;
	SigAction.sa_flags		= 0;
	sigemptyset( &SigAction.sa_mask );
	sigaction(SIGCHLD, &SigAction, 0);

	SigAction.sa_handler	= HandleSignal;
	SigAction.sa_flags		= DefaultFlag;
	sigemptyset( &SigAction.sa_mask );
	sigaction( SIGHUP, &SigAction, 0 );

	SigAction.sa_handler	= HandleSignal;
	SigAction.sa_flags		= DefaultFlag;
	sigemptyset( &SigAction.sa_mask );
	sigaction( SIGQUIT, &SigAction, 0 );

	SigAction.sa_handler	= HandleSignal;
	SigAction.sa_flags		= DefaultFlag;
	sigemptyset( &SigAction.sa_mask );
	sigaction( SIGTRAP, &SigAction, 0 );

	SigAction.sa_handler	= HandleSignal;
	SigAction.sa_flags		= DefaultFlag;
	sigemptyset( &SigAction.sa_mask );
	sigaction( SIGIOT, &SigAction, 0 );

	SigAction.sa_handler	= HandleSignal;
	SigAction.sa_flags		= DefaultFlag;
	sigemptyset( &SigAction.sa_mask );
	sigaction( SIGBUS, &SigAction, 0 );

	SigAction.sa_handler	= HandleSignal;
	SigAction.sa_flags		= DefaultFlag;
	sigemptyset( &SigAction.sa_mask );
	sigaction( SIGFPE, &SigAction, 0 );

	SigAction.sa_handler	= HandleSignal;
	SigAction.sa_flags		= DefaultFlag;
	sigemptyset( &SigAction.sa_mask );
	sigaction( SIGTERM, &SigAction, 0 );

	SigAction.sa_handler	= HandleInterrupt;
	SigAction.sa_flags		= DefaultFlag;
	sigemptyset( &SigAction.sa_mask );
	sigaction( SIGINT, &SigAction, 0 );

	signal( SIGPIPE, SIG_IGN );

    // attempt to change max file handles for this process...
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != -1)
        debugf(NAME_Init, TEXT("File handle limit is soft=(%d), hard=(%d)."), (int) rl.rlim_cur, (int) rl.rlim_max);
    else
    {
        rl.rlim_cur = DESIRED_FILE_HANDLES; // wing it.
        rl.rlim_max = RLIM_INFINITY;
    }

    if ( ((rl.rlim_cur != RLIM_INFINITY) && (rl.rlim_cur < DESIRED_FILE_HANDLES)) ||
         ((rl.rlim_max != RLIM_INFINITY) && (rl.rlim_max < DESIRED_FILE_HANDLES)) )
    {
        bool changed = false;

        if ((rl.rlim_cur != RLIM_INFINITY) && (rl.rlim_cur < DESIRED_FILE_HANDLES))
            rl.rlim_cur = DESIRED_FILE_HANDLES;
        if ((rl.rlim_max != RLIM_INFINITY) && (rl.rlim_max < DESIRED_FILE_HANDLES))
            rl.rlim_max = RLIM_INFINITY;

        changed = (setrlimit(RLIMIT_NOFILE, &rl) != -1);
        if (!changed)
        {
            debugf(NAME_Init, TEXT("failed to set open file limit to %d, %d."), (int) rl.rlim_cur, (int) rl.rlim_max);
            rl.rlim_max = DESIRED_FILE_HANDLES;
            changed = (setrlimit(RLIMIT_NOFILE, &rl) != -1);
            if (!changed)
                debugf(NAME_Init, TEXT("failed to set open file limit to %d, %d."), (int) rl.rlim_cur, (int) rl.rlim_max);
        }

        if (changed)
        {
            getrlimit(RLIMIT_NOFILE, &rl);
            debugf(NAME_Init, TEXT("Changed file handle limit to soft=(%d), hard=(%d)."), (int) rl.rlim_cur, (int) rl.rlim_max);
        }
    }

	unguard;
}

void appPlatformPreExit()
{
}




void appPlatformExit()
{
    if (GUnixSpawnOnExit != NULL)
    {
        FString fstrcmd(GUnixSpawnOnExit);
        TArray<FString> args;

        int startstr = 0;
        int max = fstrcmd.Len();

        for (int i = 0; i < max; i++)
        {
            char ch = (char) fstrcmd[i];
            if (ch == ' ')
            {
                if ( (i == 0) ||
                     (fstrcmd[i-1] == ' ') ||
                     (fstrcmd[i-1] == '\"') ||
                     (fstrcmd[i-1] == '\'') )
                {
                    startstr = i + 1;
                    continue;
                }

    		    new(args)FString( fstrcmd.Mid(startstr, i - startstr) );
                startstr = i + 1;
            }

            else if ((ch == '\"') || (ch == '\''))
            {
                if (startstr != i)
    		        new(args)FString( fstrcmd.Mid(startstr, i - startstr) );

                int j = i + 1;
                while ((j < max) && ((char) fstrcmd[j] != ch))
                    j++;

                startstr = j + 1;

                if (j == max)  // uh...oh well.
                {
                    if (j > i + 1)
			            new(args)FString( fstrcmd.Mid(i+1) );
                    break;
                }

                if (j > i + 1)
                {
			        new(args)FString( fstrcmd.Mid(i+1, (j - i) - 1) );
                    i = j;  // will increment past delimiter in next loop.
                }
            }
        }

        if (startstr < max)
    	    new(args)FString( fstrcmd.Mid(startstr) );


        char *cmd = new char[args(0).Len() + 1];
        appToAnsi(*(args(0)), cmd);
        char **ansiargs = new char *[args.Num() + 1];
        for (int i = 0; i < args.Num(); i++)
        {
            ansiargs[i] = new char[args(i).Len() + 1];
            appToAnsi(*args(i), ansiargs[i]);
        }
        ansiargs[args.Num()] = NULL;

        int rc = fork();
        if (rc == -1)
        {
            fprintf(stderr, "fork() failed!\n errno (%d) [%s].\n",
	            errno, strerror(errno));
            _exit(42);
        }
        else if (rc > 0)  // parent: die.
        {
            _exit(0);
        }

        appSleep(1);
        execv(cmd, ansiargs);  // hail mary.

        // if you hit this...there were problems.
        fprintf(stderr, "execv() failed.\nerrno (%d) [%s].\n Command line:\n  ",
                errno, strerror(errno));

        for (int x = 0; ansiargs[x]; x++)
            fprintf(stderr, "%s ", ansiargs[x]);
        fprintf(stderr, "\n\n");

        fflush(stderr);
        _exit(42);
    }
}

/*-----------------------------------------------------------------------------
	Pathnames.
-----------------------------------------------------------------------------*/

#if __FILE_NOCASE
// See if file specified by (Path) exists with a different case. This
//  assumes that the directories specified exist with the given case, and
//  only looks at the file part of the path. The first file found via
//  readdir() is the one picked, which may not be the first in ASCII order.
//  If a file is found, (Path) is overwritten to reflect the new case, and
//  (true) is returned. If no file is found, (Path) is untouched, and (false)
//  is returned.
static bool find_alternate_filecase(char *Path)
{
    guard(find_alternate_filecase);

    bool retval = false;
    char *ptr = strrchr(Path, '/');
    char *filename = (ptr != NULL) ? ptr + 1 : Path;
    char *basedir = (ptr != NULL) ? Path : ((char *) ".");

    if (ptr != NULL)
        *ptr = '\0';  // separate dir and filename.

    // fast rejection: only check this if there's no wildcard in filename.
    if (strchr(filename, '*') == NULL)
    {
        DIR *dir = opendir(basedir);
        if (dir != NULL)
        {
            struct dirent *ent;
            while (((ent = readdir(dir)) != NULL) && (!retval))
            {
                if (strcasecmp(ent->d_name, filename) == 0)  // a match?
                {
                    strcpy(filename, ent->d_name);  // overwrite with new case.
                    retval = true;
                }
            }
            closedir(dir);
        }
    }

    if (ptr != NULL)
        *ptr = '/';  // recombine dir and filename into one path.

    return(retval);

    unguard;
}
#endif


// Convert pathname to Unix format.
char* appUnixPath( const char* Path )
{
	guard(appUnixPath);
	static char UnixPath[1024];
	char* Cur = UnixPath;
	strncpy( UnixPath, Path, 1024 );
    UnixPath[1023] = '\0';

	while( Cur = strchr( Cur, '\\' ) )
		*Cur = '/';

#if __FILE_NOCASE
    // fast rejection: only check this if the current path doesn't exist...
    if (access(UnixPath, F_OK) != 0)
    {
        Cur = UnixPath;
        if (*Cur == '/')
            Cur++;

        bool keep_looking = true;
        while (keep_looking)
        {
            Cur = strchr( Cur, '/' );
            if (Cur != NULL)
                *Cur = '\0';  // null-terminate so we have this chunk of path.

            // do insensitive check only if the current path doesn't exist...
            if (access(UnixPath, F_OK) != 0)
                keep_looking = find_alternate_filecase(UnixPath);

            if (Cur == NULL)
                keep_looking = false;
            else
            {
                *Cur = '/';   // reset string for next check.
                Cur++;
            }
        }
    }
#endif

	return UnixPath;
	unguard;
}


/*-----------------------------------------------------------------------------
	Networking.
-----------------------------------------------------------------------------*/

DWORD appGetLocalIP( void )
{
	static bool lookedUp = false;
	static DWORD LocalIP = 0;

#if !__EMSCRIPTEN__
	// A page has no address of its own.
	if( !lookedUp )
	{
		lookedUp = true;
		char Hostname[256];
		Hostname[0] = '\0';
		if (gethostname( Hostname, sizeof(Hostname) ) == 0)
		{
			check(gethostbyname_mutex != NULL);
			gethostbyname_mutex->Lock();
			struct hostent *Hostinfo = gethostbyname( Hostname );
			if (Hostinfo != NULL)
				LocalIP = *(DWORD*)Hostinfo->h_addr_list[0];
			gethostbyname_mutex->Unlock();
		}
	}
#endif

	return LocalIP;
}


/*-----------------------------------------------------------------------------
	String functions.
-----------------------------------------------------------------------------*/

int stricmp( const char* s, const char* t )
{
	int	i;
	for( i = 0; tolower(s[i]) == tolower(t[i]); i++ )
		if( s[i] == '\0' )
			return 0;
	return s[i] - t[i];
}

int strnicmp( const char* s, const char* t, int n )
{
	int	i;
	if( n <= 0 )
		return 0;
	for( i = 0; tolower(s[i]) == tolower(t[i]); i++ )
		if( (s[i] == '\0') || (i == n - 1) )
			return 0;
	return s[i] - t[i];
}

// C linkage because the Karma libraries call it too.
extern "C" char* strupr( char* s )
{
	int	i;
	for( i = 0; s[i] != '\0'; i++ )
		s[i] = toupper(s[i]);
	return s;
}


/*-----------------------------------------------------------------------------
	Stuff.
-----------------------------------------------------------------------------*/

CORE_API const void EdLoadErrorf( INT Type, const TCHAR* Fmt, ... )
{ }


#endif

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/

