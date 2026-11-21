// Client and uplink for the JSON web master, a separate UT2004-web-master project.

// Standard headers go before the engine's, whose macros (check, min, max) break them.
#define NOMINMAX
#pragma pack(push, 8)
#if __EMSCRIPTEN__
#include <emscripten/fetch.h>
#else
#include <curl/curl.h>
#include <thread>
#endif
#if __ANDROID__
#include <dirent.h>
#include <fstream>
#include <sstream>
#endif
#if __IOS__
// AppleTrust.cpp
CURLcode AppleSslContext(CURL*, void* SslCtx, void*);
#endif
#include <nlohmann/json.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#pragma pack(pop)

#include "UnIpDrv.h"

// Done gets whether the server answered 2xx, and the body.
typedef std::function<void( bool, std::string )> FHttpDone;

// The reply comes from the network, so its size and row counts are bounded.
enum
{
	MAX_RESPONSE_SIZE	= 8 * 1024 * 1024,
	MAX_WEB_SERVERS		= 2048,
	MAX_WEB_SERVER_INFO	= 64,
	MAX_WEB_NEWS		= 32,
};

// The web master's credential lasts 48 hours and an expired one drops a relayed game.
// A failure is kept for a while too, or every server browser query would ask again.
enum
{
	TURN_REFRESH_TIME	= 24 * 3600,
	TURN_RETRY_TIME		= 600,
};

#if __EMSCRIPTEN__
extern "C" void ut_webrtc_host_state( const char* Json );

static void HttpFinished( emscripten_fetch_t* Fetch )
{
	// Closing a fetch that is still running calls its error handler again
	FHttpDone* Done = (FHttpDone*)Fetch->userData;
	if( !Done )
		return;
	Fetch->userData = NULL;
	bool Ok = Fetch->status >= 200 && Fetch->status < 300 && Fetch->numBytes <= MAX_RESPONSE_SIZE;
	(*Done)( Ok, Ok ? std::string( Fetch->data, Fetch->numBytes ) : std::string() );
	delete Done;
	emscripten_fetch_close( Fetch );
}

static void HttpProgress( emscripten_fetch_t* Fetch )
{
	if( Fetch->dataOffset + Fetch->numBytes > MAX_RESPONSE_SIZE )
	{
		Fetch->status = 0;
		HttpFinished( Fetch );
	}
}

// GET only, because a GET without headers needs no CORS preflight. The callbacks run on the main thread once the browser has the answer.
static void HttpGetAsync( const std::string& Url, FHttpDone Done )
{
	emscripten_fetch_attr_t Attr;
	emscripten_fetch_attr_init( &Attr );
	strcpy( Attr.requestMethod, "GET" );
	Attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
	Attr.timeoutMSecs = 5000;
	Attr.userData = new FHttpDone( Done );
	Attr.onsuccess = HttpFinished;
	Attr.onerror = HttpFinished;
	Attr.onprogress = HttpProgress;
	emscripten_fetch( &Attr, Url.c_str() );
}
#else
static size_t AppendResponse( char* Data, size_t Size, size_t Count, void* Response )
{
	std::string* Out = (std::string*)Response;
	// A short count makes curl abort the transfer with an error
	if( Out->size() + Size * Count > MAX_RESPONSE_SIZE )
		return 0;
	Out->append( Data, Size * Count );
	return Size * Count;
}

#if __ANDROID__
// OpenSSL has no CA store on Android. The files are PEM, the text around each certificate is skipped.
const std::string& AndroidCaCerts()
{
	static const std::string Certs = []()
	{
		std::string All;
		for( const char* Dir : { "/apex/com.android.conscrypt/cacerts", "/system/etc/security/cacerts" } )
			if( DIR* D = opendir( Dir ) )
			{
				while( dirent* E = readdir( D ) )
					if( E->d_name[0] != '.' )
					{
						std::ostringstream Cert;
						Cert << std::ifstream( std::string(Dir) + "/" + E->d_name ).rdbuf();
						All += Cert.str() + "\n";
					}
				closedir( D );
				if( !All.empty() )
					break;
			}
		return All;
	}();
	return Certs;
}
#endif

// Runs on a worker thread, so it only touches standard types.
static bool HttpRequest( const std::string& Url, const std::string& Body, std::string& Response )
{
	CURL* Curl = curl_easy_init();
	if( !Curl )
		return false;
	curl_slist* Headers = curl_slist_append( NULL, "Content-Type: application/json" );
	curl_easy_setopt( Curl, CURLOPT_URL, Url.c_str() );
	curl_easy_setopt( Curl, CURLOPT_HTTPHEADER, Headers );
	// The master lists the address a post comes from, and the game is reachable over IPv4 only.
	curl_easy_setopt( Curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4 );
	// An offer waits up to 8 seconds for the host to answer, an unreachable master still fails fast
	curl_easy_setopt( Curl, CURLOPT_CONNECTTIMEOUT, 5L );
	curl_easy_setopt( Curl, CURLOPT_TIMEOUT, 10L );
	// Schannel otherwise fails when the CA's revocation server is unreachable, WinHTTP never checked it.
	curl_easy_setopt( Curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_REVOKE_BEST_EFFORT );
#if __ANDROID__
	curl_blob Ca = { (void*)AndroidCaCerts().data(), AndroidCaCerts().size(), CURL_BLOB_NOCOPY };
	curl_easy_setopt( Curl, CURLOPT_CAINFO_BLOB, &Ca );
#elif __IOS__
	curl_easy_setopt( Curl, CURLOPT_SSL_CTX_FUNCTION, AppleSslContext );
#endif
	curl_easy_setopt( Curl, CURLOPT_WRITEFUNCTION, AppendResponse );
	curl_easy_setopt( Curl, CURLOPT_WRITEDATA, &Response );
	if( !Body.empty() )
	{
		curl_easy_setopt( Curl, CURLOPT_POSTFIELDS, Body.data() );
		curl_easy_setopt( Curl, CURLOPT_POSTFIELDSIZE, (long)Body.size() );
	}
	long Status = 0;
	bool Ok = curl_easy_perform( Curl ) == CURLE_OK
		&& curl_easy_getinfo( Curl, CURLINFO_RESPONSE_CODE, &Status ) == CURLE_OK
		&& Status >= 200 && Status < 300;
	curl_slist_free_all( Headers );
	curl_easy_cleanup( Curl );
	return Ok;
}

// Done runs on the worker thread.
static void HttpRequestAsync( const std::string& Url, const std::string& Body, FHttpDone Done )
{
	std::thread( [Url, Body, Done]()
	{
		std::string Response;
		bool Ok = HttpRequest( Url, Body, Response );
		Done( Ok, std::move(Response) );
	} ).detach();
}
#endif

FString WebMasterURL()
{
	FString Url;
	GConfig->GetString( TEXT("IpDrv.MasterServerLink"), TEXT("WebMasterServer"), Url );
	while( Url.Right(1) == TEXT("/") )
		Url = Url.LeftChop(1);
	return Url;
}

void WebMasterAdvertise( const FServerResponseLine& Server )
{
	FString Base = WebMasterURL();
	if( !Base.Len() )
		return;

	nlohmann::json Info = nlohmann::json::array();
	for( INT i=0; i<Server.ServerInfo.Num(); i++ )
		Info.push_back( { { "key", &appToUtf8(*Server.ServerInfo(i).Key)(0) }, { "value", &appToUtf8(*Server.ServerInfo(i).Value)(0) } } );
	nlohmann::json Body =
	{
		{ "port", Server.Port },
		{ "name", &appToUtf8(*Server.ServerName)(0) },
		{ "map", &appToUtf8(*Server.MapName)(0) },
		{ "gameType", &appToUtf8(*Server.GameType)(0) },
		{ "players", Server.CurrentPlayers },
		{ "maxPlayers", Server.MaxPlayers },
		{ "flags", Server.Flags },
		{ "skill", &appToUtf8(*Server.SkillLevel)(0) },
		{ "info", Info },
	};

	std::string Json = Body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
#if __EMSCRIPTEN__
	// A browser host is listed through its signaling socket, a JSON post would need a CORS preflight
	ut_webrtc_host_state( Json.c_str() );
#else
	HttpRequestAsync( &appToUtf8( *(Base + TEXT("/advertise")) )(0), Json, []( bool, std::string ) {} );
#endif
}

#pragma pack(push, 8)
struct FWebMasterResponse
{
	std::mutex Lock;
	bool Done = false;
	bool Ok = false;
	std::string Body;
};
#pragma pack(pop)

// The request holds its own reference, so freeing one that is still running is safe.
struct FWebMasterRequest
{
	std::shared_ptr<FWebMasterResponse> Response;
};

static FWebMasterRequest* WebMasterRequest( const TCHAR* Path, const std::string& Body )
{
	FString Base = WebMasterURL();
	FWebMasterRequest* Request = new FWebMasterRequest;
	Request->Response = std::make_shared<FWebMasterResponse>();
	if( !Base.Len() )
	{
		debugf( TEXT("WebMasterServer is not set in [IpDrv.MasterServerLink]") );
		Request->Response->Done = true;
		return Request;
	}
	FHttpDone Done = [Response = Request->Response]( bool Ok, std::string Body )
	{
		std::lock_guard<std::mutex> Guard( Response->Lock );
		Response->Body.swap( Body );
		Response->Ok = Ok;
		Response->Done = true;
	};
#if __EMSCRIPTEN__
	HttpGetAsync( &appToUtf8( *(Base + Path) )(0), Done );
#else
	HttpRequestAsync( &appToUtf8( *(Base + Path) )(0), Body, Done );
#endif
	return Request;
}

static UBOOL WebMasterRequestDone( FWebMasterRequest* Request, UBOOL& Ok, std::string& Body )
{
	std::lock_guard<std::mutex> Guard( Request->Response->Lock );
	if( !Request->Response->Done )
		return 0;
	Ok = Request->Response->Ok;
	Body.swap( Request->Response->Body );
	return 1;
}

FWebMasterRequest* WebMasterList()
{
	return WebMasterRequest( TEXT("/"), std::string() );
}

#if !__EMSCRIPTEN__
FWebMasterRequest* WebMasterOffer( const FString& HostId, const FString& Sdp )
{
	return WebMasterRequest( TEXT("/offer"), nlohmann::json{ { "host", &appToUtf8(*HostId)(0) }, { "sdp", &appToUtf8(*Sdp)(0) } }.dump() );
}

UBOOL WebMasterOfferDone( FWebMasterRequest* Request, UBOOL& Ok, FString& Sdp )
{
	std::string Body;
	if( !WebMasterRequestDone( Request, Ok, Body ) )
		return 0;
	nlohmann::json Root = nlohmann::json::parse( Body, nullptr, false );
	Ok = Ok && Root.is_object() && Root.contains("sdp") && Root["sdp"].is_string();
	if( Ok )
		Sdp = appFromUtf8( Root["sdp"].get<std::string>().c_str() );
	return 1;
}

// The first UDP relay of the list, the only kind the game's ICE library can use
static void ParseTurn( const std::string& Body, std::string& Host, INT& Port, std::string& Username, std::string& Password )
{
	nlohmann::json Root = nlohmann::json::parse( Body, nullptr, false );
	if( !Root.is_object() || !Root.contains("iceServers") || !Root["iceServers"].is_array() )
		return;
	for( const auto& Server : Root["iceServers"] )
	{
		if( !Server.is_object() || !Server.contains("urls") || !Server["urls"].is_array()
		||	!Server.contains("username") || !Server["username"].is_string()
		||	!Server.contains("credential") || !Server["credential"].is_string() )
			continue;
		for( const auto& Item : Server["urls"] )
		{
			if( !Item.is_string() )
				continue;
			std::string Url = Item.get<std::string>();
			size_t Colon = Url.find( ':', 5 );
			if( Url.compare( 0, 5, "turn:" ) != 0 || Colon == std::string::npos || Url.find("transport=tcp") != std::string::npos )
				continue;
			INT UrlPort = atoi( Url.c_str() + Colon + 1 );
			if( Colon == 5 || UrlPort < 1 || UrlPort > 65535 )
				continue;
			Host = Url.substr( 5, Colon - 5 );
			Port = UrlPort;
			Username = Server["username"].get<std::string>();
			Password = Server["credential"].get<std::string>();
			return;
		}
	}
}

UBOOL WebMasterTurn( FWebMasterTurn& Out )
{
	// Standard strings, these outlive the engine's allocator
	static FWebMasterRequest* Request;
	static std::string Host, Username, Password;
	static INT Port;
	static DOUBLE NextTime;
	if( Request )
	{
		UBOOL Ok;
		std::string Body;
		if( !WebMasterRequestDone( Request, Ok, Body ) )
			return 0;
		WebMasterRequestFree( Request );
		Request = nullptr;
		Host.clear();
		if( Ok )
			ParseTurn( Body, Host, Port, Username, Password );
		NextTime = appSeconds() + (Host.empty() ? TURN_RETRY_TIME : TURN_REFRESH_TIME);
	}
	else if( appSeconds() >= NextTime )
	{
		Request = WebMasterRequest( TEXT("/ice"), std::string() );
		return 0;
	}
	Out.Host = Host.c_str();
	Out.Username = Username.c_str();
	Out.Password = Password.c_str();
	Out.Port = Port;
	return 1;
}
#endif

// The server browser builds the join URL from these, so nothing that could start another URL part may pass.
static UBOOL IsDottedQuad( const TCHAR* S )
{
	for( INT Part=0; Part<4; Part++ )
	{
		INT Value = 0, Digits = 0;
		for( ; *S>='0' && *S<='9' && Digits<3; S++, Digits++ )
			Value = Value*10 + (*S-'0');
		if( !Digits || Value>255 || (Digits>1 && S[-Digits]=='0') || *S++ != (Part<3 ? '.' : 0) )
			return 0;
	}
	return 1;
}

UBOOL WebMasterListDone( FWebMasterRequest* Request, UBOOL& Ok, TArray<FServerResponseLine>& Servers, FString& News )
{
	std::string Response;
	if( !WebMasterRequestDone( Request, Ok, Response ) )
		return 0;

	nlohmann::json Root = nlohmann::json::parse( Response, nullptr, false );
	if( !Ok || !Root.is_object() )
	{
		Ok = 0;
		return 1;
	}

	auto Str = []( const nlohmann::json& J, const char* Key ) { return J.contains(Key) && J[Key].is_string() ? appFromUtf8( J[Key].get<std::string>().c_str() ) : FString(); };
	auto Int = []( const nlohmann::json& J, const char* Key ) { return J.contains(Key) && J[Key].is_number_integer() ? J[Key].get<INT>() : 0; };
	if( Root.contains("servers") && Root["servers"].is_array() )
		for( const nlohmann::json& S : Root["servers"] )
		{
			if( Servers.Num() >= MAX_WEB_SERVERS )
				break;
			FString IP = Str( S, "ip" );
			INT Port = Int( S, "port" );
			if( !S.is_object() || !IsDottedQuad(*IP) || Port<1 || Port>65535 )
				continue;
			FServerResponseLine* Server = &Servers(Servers.AddZeroed());
			Server->IP = IP;
			Server->Port = Port;
			Server->QueryPort = Server->Port + 1;
			Server->ServerName = Str( S, "name" );
			Server->MapName = Str( S, "map" );
			Server->GameType = Str( S, "gameType" );
			Server->CurrentPlayers = Int( S, "players" );
			Server->MaxPlayers = Int( S, "maxPlayers" );
			Server->Flags = Int( S, "flags" );
			Server->SkillLevel = Str( S, "skill" );
			if( S.contains("info") && S["info"].is_array() )
				for( const nlohmann::json& P : S["info"] )
					if( P.is_object() && Server->ServerInfo.Num() < MAX_WEB_SERVER_INFO )
					{
						FKeyValuePair& Pair = Server->ServerInfo(Server->ServerInfo.AddZeroed());
						Pair.Key = Str( P, "key" );
						Pair.Value = Str( P, "value" );
					}
		}
	INT NewsCount = 0;
	if( Root.contains("news") && Root["news"].is_array() )
		for( const nlohmann::json& N : Root["news"] )
			if( N.is_object() && NewsCount++ < MAX_WEB_NEWS )
				News += Str( N, "title" ) + TEXT("\r") + Str( N, "body" ) + TEXT("\r\r");
	return 1;
}

void WebMasterRequestFree( FWebMasterRequest* Request )
{
	delete Request;
}

