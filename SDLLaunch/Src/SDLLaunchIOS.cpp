// The iOS app. The System and script files ship in the app bundle and are copied to the app's Documents
// folder, where the engine writes its inis. The content is downloaded on first launch or comes from an
// archive or game folder the player puts into that folder with the Files app or a computer.

// Standard and library headers go before the engine's, whose macros break them.
#include <time.h>  // dodging a macro clash with Unix C runtime headers...
#include "SDLLaunchContent.h"

#pragma pack(push,8)
#include <algorithm>
#include <AL/alc.h>
#include <AL/alext.h>
#include <GLES3/gl3.h>
#include <curl/curl.h>
#include <openssl/evp.h>
#pragma pack(pop)
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include "SDLLaunchPrivate.h"

// IpDrv/Src/AppleTrust.cpp
CURLcode AppleSslContext(CURL*, void* SslCtx, void*);

static bool IOSFail(const char* Fmt, const char* Arg)
{
    char Msg[1024];
    SDL_snprintf(Msg, sizeof(Msg), Fmt, Arg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "UT2004", Msg, NULL);
    return false;
}

/*-----------------------------------------------------------------------------
	Splash.
-----------------------------------------------------------------------------*/

// iOS shows one window, so SDLDrv takes this one over and the logo stays up until the game draws.
// The renderer is OpenGL ES (see main), so the window is made for the GL ES 3 context OpenGLDrv asks for.
void InitSplash(const TCHAR *filename)
{
    debugf(TEXT("iOS: splash %s"), filename ? filename : TEXT("(none)"));
    SDL_Surface *bmp = filename ? SDL_LoadBMP(appToAnsi(filename)) : NULL;
    // The engine finds files whatever their case, SDL doesn't, and the iPhone's file system tells cases apart.
    // The logo ships as Help/ut2004logo.bmp while the engine asks for UT2004Logo.bmp.
    if (bmp == NULL && filename)
    {
        FString Path(filename);
        INT Slash = Path.InStr(TEXT("/"), 1);
        FString Lower = Path.Left(Slash + 1) + Path.Mid(Slash + 1).Locs();
        bmp = SDL_LoadBMP(appToAnsi(*Lower));
    }
    if (bmp == NULL || !SDL_Init(SDL_INIT_VIDEO))
    {
        debugf(TEXT("iOS: no splash: %s"), appFromAnsi(SDL_GetError()));
        SDL_DestroySurface(bmp);
        return;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    // SDLDrv destroys the renderer when it adopts the window. Without high pixel density the game would
    // draw at a third of the resolution on most iPhones, ResolutionScale is how the player trades it for speed.
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    if (SDL_CreateWindowAndRenderer("UT2004", 0, 0, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY, &window, &renderer))
    {
        SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, bmp);
        int w, h;
        SDL_GetCurrentRenderOutputSize(renderer, &w, &h);
        float scale = SDL_min(0.6f * w / bmp->w, 0.6f * h / bmp->h);
        SDL_FRect dest = { (w - bmp->w * scale) / 2, (h - bmp->h * scale) / 2, bmp->w * scale, bmp->h * scale };
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderTexture(renderer, texture, NULL, &dest);
        SDL_RenderPresent(renderer);
        SDL_DestroyTexture(texture);
        debugf(TEXT("iOS: splash shown, %ix%i pixels, renderer %s"), w, h, appFromAnsi(SDL_GetRendererName(renderer)));
    }
    else
        debugf(TEXT("iOS: no splash window: %s"), appFromAnsi(SDL_GetError()));
    SDL_DestroySurface(bmp);
}

/*-----------------------------------------------------------------------------
	Background.
-----------------------------------------------------------------------------*/

static SDL_AtomicInt InBackground;

// SDL calls this on the main thread from inside SDL_PumpEvents, the engine may be anywhere in a frame then.
static bool SDLCALL WatchApp(void*, SDL_Event* Event)
{
    if (Event->type == SDL_EVENT_WILL_ENTER_BACKGROUND)
        SDL_SetAtomicInt(&InBackground, 1);
    else if (Event->type == SDL_EVENT_DID_ENTER_FOREGROUND)
        SDL_SetAtomicInt(&InBackground, 0);
    else if (Event->type == SDL_EVENT_TERMINATING)
        GIsRequestingExit = 1;
    return true;
}

// iOS ends an app that uses OpenGL in the background, so the frame loop waits here until it is back.
/*-----------------------------------------------------------------------------
	Hang watchdog.
-----------------------------------------------------------------------------*/

// When no frame starts for 5 seconds, the main thread writes where it is to System/Hang.log and
// carries on. A frozen picture with the sound still playing is the main thread stuck somewhere.
static SDL_AtomicU32 FrameTicks;
static pthread_t MainThread;
static int HangFd = -1;

static void WriteHang(const char* Text)
{
    if (HangFd >= 0)
        write(HangFd, Text, SDL_strlen(Text));
}

// Runs on the main thread, interrupting whatever it was stuck in. backtrace_symbols_fd doesn't allocate.
static void HangSignal(int)
{
    void* Frames[96];
    int Count = backtrace(Frames, SDL_arraysize(Frames));
    WriteHang("Main thread:\n");
    backtrace_symbols_fd(Frames, Count, HangFd);
    WriteHang("\n");
}

static int SDLCALL RunWatchdog(void*)
{
    Uint32 Reported = 0;
    int Times = 0;
    for (;;)
    {
        SDL_Delay(1000);
        Uint32 Last = SDL_GetAtomicU32(&FrameTicks), Now = (Uint32)SDL_GetTicks();
        if (!Last || Now - Last < 5000 || SDL_GetAtomicInt(&InBackground))
            continue;
        // Once 5 s into a stall and again at 20 s, to tell a stall that moves from one that doesn't.
        if (Reported != Last)
            Reported = Last, Times = 0;
        if (Times == 2 || (Times == 1 && Now - Last < 20000))
            continue;
        Times++;
        char Line[160];
        SDL_snprintf(Line, sizeof(Line), "No frame for %u ms (%u s after start), report %d:\n", Now - Last, Now / 1000, Times);
        WriteHang(Line);
        pthread_kill(MainThread, SIGUSR2);
    }
    return 0;
}

static void StartWatchdog()
{
    MainThread = pthread_self();
    HangFd = open("Hang.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    struct sigaction Action = {};
    Action.sa_handler = HangSignal;
    sigemptyset(&Action.sa_mask);
    Action.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &Action, NULL);
    SDL_DetachThread(SDL_CreateThread(RunWatchdog, "Watchdog", NULL));
}

/*-----------------------------------------------------------------------------
	Background.
-----------------------------------------------------------------------------*/

void WaitWhileInBackground()
{
    // Called at the start of every frame.
    SDL_SetAtomicU32(&FrameTicks, (Uint32)SDL_GetTicks() | 1);
    if (!SDL_GetAtomicInt(&InBackground))
        return;
    if (SDL_GL_GetCurrentContext())
        glFinish();
    // A suspended app can be ended without notice, so the settings are saved now.
    if (GConfig)
        GConfig->Flush(0);
    // Core Audio stops for a suspended app and OpenAL Soft would not pick up again after a call took the audio session.
    ALCcontext* Context = alcGetCurrentContext();
    ALCdevice* Device = Context ? alcGetContextsDevice(Context) : NULL;
    LPALCDEVICEPAUSESOFT Pause = NULL;
    LPALCDEVICERESUMESOFT Resume = NULL;
    if (Device && alcIsExtensionPresent(Device, "ALC_SOFT_pause_device"))
    {
        Pause = (LPALCDEVICEPAUSESOFT)alcGetProcAddress(Device, "alcDevicePauseSOFT");
        Resume = (LPALCDEVICERESUMESOFT)alcGetProcAddress(Device, "alcDeviceResumeSOFT");
    }
    if (Pause && Resume)
        Pause(Device);
    debugf(TEXT("iOS: in the background, waiting"));
    while (SDL_GetAtomicInt(&InBackground) && !GIsRequestingExit)
    {
        SDL_PumpEvents();
        appSleep(0.05f);
    }
    if (Pause && Resume)
        Resume(Device);
    debugf(TEXT("iOS: back in the foreground"));
}

/*-----------------------------------------------------------------------------
	System and script files.
-----------------------------------------------------------------------------*/

static bool CopyBundleFile(const char* Src, const char* Dest)
{
    static uint8_t Buf[1 << 18];
    char Dir[1024];
    SDL_strlcpy(Dir, Dest, sizeof(Dir));
    *SDL_strrchr(Dir, '/') = 0;
    SDL_CreateDirectory(Dir);
    SDL_IOStream* In = SDL_IOFromFile(Src, "rb");
    SDL_IOStream* Out = In ? SDL_IOFromFile(Dest, "wb") : NULL;
    bool Ok = Out != NULL;
    size_t Got;
    while (Ok && (Got = SDL_ReadIO(In, Buf, sizeof(Buf))) > 0)
        Ok = SDL_WriteIO(Out, Buf, Got) == Got;
    if (In)
        SDL_CloseIO(In);
    if (Out && !SDL_CloseIO(Out))
        Ok = false;
    return Ok;
}

// The engine writes next to its files, which the signed bundle doesn't allow, so they are copied once per app build.
static bool InstallBundleFiles(const char* Root)
{
    const char* Bundle = SDL_GetBasePath();
    char Path[1024];
    // In a folder of its own, since the files go into a UT2004 folder and UT2004 is the executable's name.
    SDL_snprintf(Path, sizeof(Path), "%sGame/manifest.txt", Bundle ? Bundle : "");
    char* Manifest = Bundle ? (char*)SDL_LoadFile(Path, NULL) : NULL;
    char* Rel = Manifest ? SDL_strchr(Manifest, '\n') : NULL;
    if (!Rel)
        return IOSFail("The app has no %s, it was built without its game files.", "manifest.txt");
    *Rel++ = 0;

    char StampPath[1024];
    SDL_snprintf(StampPath, sizeof(StampPath), "%s/assets.stamp", Root);
    char* Stamp = (char*)SDL_LoadFile(StampPath, NULL);
    bool Ok = Stamp && SDL_strcmp(Stamp, Manifest) == 0;
    SDL_free(Stamp);
    if (Ok)
    {
        SDL_free(Manifest);
        return true;
    }

    INT Total = 0;
    for (const char* C = Rel; *C; C++)
        Total += *C == '\n';
    Ok = true;
    for (INT Done = 0; Ok && *Rel; Done++)
    {
        char* End = SDL_strchr(Rel, '\n');
        *End = 0;
        DrawProgress("Installing game files", (float)Done / Total);
        char Src[1024], Dest[1024];
        SDL_snprintf(Src, sizeof(Src), "%sGame/%s", Bundle, Rel);
        SDL_snprintf(Dest, sizeof(Dest), "%s/%s", Root, Rel);
        Ok = CopyBundleFile(Src, Dest);
        if (!Ok)
            IOSFail("Couldn't install %s. Is the device out of storage?", Rel);
        Rel = End + 1;
    }
    // Written last, so an interrupted copy starts over on the next launch.
    if (Ok)
        SDL_SaveFile(StampPath, Manifest, SDL_strlen(Manifest));
    SDL_free(Manifest);
    return Ok;
}

/*-----------------------------------------------------------------------------
	Content.
-----------------------------------------------------------------------------*/

static bool HasMaps(const char* ContentRoot)
{
    int Count = 0;
    char** Found = SDL_GlobDirectory(ContentRoot, "Maps/*.ut2", SDL_GLOB_CASEINSENSITIVE, &Count);
    SDL_free(Found);
    return Count > 0;
}

// The download streams from a thread through a pipe into the unpacker, so the device needs no room for the archive.
struct FStreamDownload
{
    const char* Url;
    CURL* Curl;
    int Fd;
    FInstall* I;
    EVP_MD_CTX* Sha;
    CURLcode Code;
    char Reason[CURL_ERROR_SIZE];
};

static size_t WriteToPipe(char* Data, size_t Size, size_t Count, void* User)
{
    FStreamDownload* D = (FStreamDownload*)User;
    // Set before the first bytes reach the reader, which draws the progress from it.
    if (D->I->Size <= 0)
    {
        curl_off_t Length = -1;
        if (curl_easy_getinfo(D->Curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &Length) == CURLE_OK && Length > 0)
            D->I->Size = Length;
    }
    size_t Total = Size * Count;
    if (D->Sha)
        EVP_DigestUpdate(D->Sha, Data, Total);
    for (size_t Left = Total; Left; )
    {
        ssize_t Wrote = write(D->Fd, Data, Left);
        if (Wrote < 0 && errno == EINTR)
            continue;
        // The reader stopped. The pipe is set not to raise SIGPIPE.
        if (Wrote <= 0)
            return 0;
        Data += Wrote;
        Left -= Wrote;
    }
    return Total;
}

static int SDLCALL RunDownload(void* User)
{
    FStreamDownload* D = (FStreamDownload*)User;
    D->Curl = curl_easy_init();
    D->Code = CURLE_FAILED_INIT;
    if (D->Curl)
    {
        curl_easy_setopt(D->Curl, CURLOPT_URL, D->Url);
        curl_easy_setopt(D->Curl, CURLOPT_FOLLOWLOCATION, 1L);
        // An error page must not end up in the unpacker.
        curl_easy_setopt(D->Curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(D->Curl, CURLOPT_CONNECTTIMEOUT, 20L);
        curl_easy_setopt(D->Curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(D->Curl, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(D->Curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(D->Curl, CURLOPT_ERRORBUFFER, D->Reason);
        curl_easy_setopt(D->Curl, CURLOPT_SSL_CTX_FUNCTION, AppleSslContext);
        curl_easy_setopt(D->Curl, CURLOPT_WRITEFUNCTION, WriteToPipe);
        curl_easy_setopt(D->Curl, CURLOPT_WRITEDATA, D);
        D->Code = curl_easy_perform(D->Curl);
        curl_easy_cleanup(D->Curl);
    }
    // The reader sees the end of the stream.
    close(D->Fd);
    return 0;
}

static bool DownloadAndUnpack(FInstall& I)
{
    int Pipe[2];
    if (pipe(Pipe) != 0)
        return I.Fail("Couldn't start the download of %s.");
    // A reader that stops early must not take the app down with SIGPIPE.
    fcntl(Pipe[1], F_SETNOSIGPIPE, 1);
    FStreamDownload D = {};
    D.Url = I.Src;
    D.Fd = Pipe[1];
    D.I = &I;
    // Only the built-in download has a known digest, anything else is the player's own choice.
    bool Checked = UT_ASSETS_SHA256[0] && SDL_strcmp(I.Src, UT_ASSETS_URL) == 0;
    D.Sha = Checked ? EVP_MD_CTX_new() : NULL;
    if (D.Sha && !EVP_DigestInit_ex(D.Sha, EVP_sha256(), NULL))
    {
        EVP_MD_CTX_free(D.Sha);
        D.Sha = NULL;
        Checked = false;
    }
    I.Verb = "Downloading the game content";
    I.Draw();
    SDL_Thread* Thread = SDL_CreateThread(RunDownload, "Download", &D);
    if (!Thread)
    {
        close(Pipe[0]);
        close(Pipe[1]);
        EVP_MD_CTX_free(D.Sha);
        return I.Fail("Couldn't start the download of %s.");
    }

    std::vector<std::string> Created;
    CreatedFiles = Checked ? &Created : NULL;
    bool Ok = UnpackStream(Pipe[0], I);
    CreatedFiles = NULL;
    // The digest covers every byte of the download, which may go on past the end of the archive.
    static uint8_t Buf[1 << 16];
    if (Ok)
        while (!SetupQuit && (read(Pipe[0], Buf, sizeof(Buf)) > 0 || errno == EINTR))
            errno = 0;
    // Closing the read end ends a download the unpacker gave up on.
    close(Pipe[0]);
    SDL_WaitThread(Thread, NULL);

    if (Ok && D.Code != CURLE_OK)
    {
        SDL_snprintf(I.Err, sizeof(I.Err), "Couldn't download %s:\n%s", I.Src, D.Reason[0] ? D.Reason : curl_easy_strerror(D.Code));
        Ok = false;
    }
    else if (!Ok && D.Code != CURLE_OK && D.Code != CURLE_WRITE_ERROR)
        SDL_snprintf(I.Err, sizeof(I.Err), "Couldn't download %s:\n%s", I.Src, D.Reason[0] ? D.Reason : curl_easy_strerror(D.Code));
    if (Checked)
    {
        unsigned char Digest[EVP_MAX_MD_SIZE];
        unsigned int Len = 0;
        char Hex[2 * EVP_MAX_MD_SIZE + 1] = "";
        if (Ok && EVP_DigestFinal_ex(D.Sha, Digest, &Len))
            for (unsigned int i = 0; i < Len; i++)
                SDL_snprintf(Hex + 2 * i, 3, "%02x", Digest[i]);
        if (!(Ok && SDL_strcasecmp(Hex, UT_ASSETS_SHA256) == 0))
        {
            // Only what this download wrote goes, the folders may hold files the player put there.
            for (const std::string& Path : Created)
                SDL_RemovePath(Path.c_str());
            if (Ok)
                Ok = I.Fail("%s is not the published game content, its SHA-256 does not match.");
        }
    }
    EVP_MD_CTX_free(D.Sha);
    return Ok;
}

static bool IsArchive(const char* Name)
{
    static const char* Exts[] = { ".zip", ".tar", ".tar.xz", ".txz" };
    size_t Len = SDL_strlen(Name);
    for (const char* Ext : Exts)
    {
        size_t ExtLen = SDL_strlen(Ext);
        if (Len > ExtLen && SDL_strcasecmp(Name + Len - ExtLen, Ext) == 0)
            return true;
    }
    return false;
}

struct FDocumentScan
{
    const char* Root;
    // Large content comes in parts, as zips of the same folders.
    std::vector<std::string> Archives;
    char Folder[1024];
};

static SDL_EnumerationResult SDLCALL ScanDocument(void* User, const char* Dir, const char* Name)
{
    FDocumentScan* Scan = (FDocumentScan*)User;
    char Path[1024];
    SDL_snprintf(Path, sizeof(Path), "%s%s", Dir, Name);
    SDL_PathInfo Info;
    if (!SDL_GetPathInfo(Path, &Info))
        return SDL_ENUM_CONTINUE;
    if (Info.type == SDL_PATHTYPE_FILE && IsArchive(Name))
        Scan->Archives.push_back(Path);
    // A game folder copied in whole. UT2004 is where the content goes, Scripts and Inbox are the app's own.
    else if (Info.type == SDL_PATHTYPE_DIRECTORY && !Scan->Folder[0]
        && SDL_strcasecmp(Name, "UT2004") && SDL_strcasecmp(Name, "Scripts") && SDL_strcasecmp(Name, "Inbox") && HasMaps(Path))
        SDL_strlcpy(Scan->Folder, Path, sizeof(Scan->Folder));
    return SDL_ENUM_CONTINUE;
}

static SDL_EnumerationResult SDLCALL MoveContentDir(void* User, const char* Dir, const char* Name)
{
    static const char* ContentDirs[] = { "Animations", "Maps", "Music", "Sounds", "StaticMeshes", "Textures" };
    const char* ContentRoot = (const char*)User;
    for (const char* Wanted : ContentDirs)
        if (SDL_strcasecmp(Name, Wanted) == 0)
        {
            char From[1024], To[1024];
            SDL_snprintf(From, sizeof(From), "%s%s", Dir, Name);
            SDL_snprintf(To, sizeof(To), "%s/%s", ContentRoot, Wanted);
            // Both are in the app's folder, so this is a rename and takes no space.
            if (!SDL_GetPathInfo(To, NULL))
                SDL_RenamePath(From, To);
        }
    return SDL_ENUM_CONTINUE;
}

// Takes the game folder or the archives the player copied into the app's Documents folder.
static bool InstallFromDocuments(const char* Root, FInstall& I)
{
    FDocumentScan Scan = {};
    Scan.Root = Root;
    SDL_EnumerateDirectory(Root, ScanDocument, &Scan);
    // Files opened with the app from another one land in Inbox.
    char Inbox[1024];
    SDL_snprintf(Inbox, sizeof(Inbox), "%s/Inbox", Root);
    SDL_EnumerateDirectory(Inbox, ScanDocument, &Scan);
    if (Scan.Folder[0])
    {
        SDL_CreateDirectory(I.Root);
        SDL_EnumerateDirectory(Scan.Folder, MoveContentDir, (void*)I.Root);
    }
    if (Scan.Archives.empty())
    {
        if (HasMaps(I.Root))
            return true;
        SDL_snprintf(I.Err, sizeof(I.Err),
            "No game content was found in the app's folder.\n\n"
            "Open the Files app, go to On My iPhone (or iPad) > UT2004 and copy in the folder of a UT2004 install, the one that holds "
            "Maps and Textures, or a .zip, .tar or .tar.xz archive of it. A computer can copy it there through Finder or iTunes too.");
        return false;
    }
    // In name order, so the parts of a split download go in their order.
    std::sort(Scan.Archives.begin(), Scan.Archives.end());
    char Verb[128];
    bool Ok = true;
    for (size_t i = 0; Ok && !SetupQuit && i < Scan.Archives.size(); i++)
    {
        const char* Archive = Scan.Archives[i].c_str();
        I.Src = Archive;
        int Fd = open(Archive, O_RDONLY);
        if (Fd < 0)
        {
            Ok = I.Fail("Couldn't open %s.");
            break;
        }
        struct stat St;
        I.Size = fstat(Fd, &St) == 0 ? (Sint64)St.st_size : 0;
        I.Done = I.Drawn = 0;
        if (Scan.Archives.size() > 1)
            SDL_snprintf(Verb, sizeof(Verb), "Unpacking part %d of %d", (int)i + 1, (int)Scan.Archives.size());
        else
            SDL_strlcpy(Verb, "Unpacking the game content", sizeof(Verb));
        I.Verb = Verb;
        I.Draw();
        int FilesBefore = I.Files;
        Ok = UnpackStream(Fd, I);
        close(Fd);
        if (Ok && I.Files == FilesBefore)
            Ok = I.Fail("No Animations, Maps, Music, Sounds, StaticMeshes or Textures folders were found in %s.");
        // Each archive is gigabytes the device would keep for nothing.
        if (Ok)
            SDL_RemovePath(Archive);
    }
    // The names die with Scan, the caller records the source as the Documents folder.
    I.Src = NULL;
    return Ok && !SetupQuit;
}

// The content is too large for the app, so the player picks on first launch where it comes from.
static bool InstallContent(const char* Root)
{
    char StampPath[1024], ContentRoot[1024];
    SDL_snprintf(StampPath, sizeof(StampPath), "%s/content.stamp", Root);
    SDL_snprintf(ContentRoot, sizeof(ContentRoot), "%s/UT2004", Root);
    if (SDL_GetPathInfo(StampPath, NULL))
        return true;
    // A player who copied the folders in before the first launch.
    if (HasMaps(ContentRoot))
        return SDL_SaveFile(StampPath, "Documents", 9), true;

    enum { Quit, Reduced, Full, Files };
    static const SDL_MessageBoxButtonData Buttons[] = {
        { 0, Reduced, "Ten maps (1 GB download)" },
        { 0, Full, "Full game (3.8 GB download, needs 13 GB free)" },
        { 0, Files, "Use files I copied in" },
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, Quit, "Quit" },
    };
    char Message[2048] = "Choose where the game content comes from.\n\n"
        "The download comes from the project's content host or the Internet Archive. "
        "Or copy the folder or an archive of a UT2004 install into On My iPhone (or iPad) > UT2004 with the Files app.";
    for (;;)
    {
        SDL_MessageBoxData Box = { SDL_MESSAGEBOX_INFORMATION, NULL, "UT2004 game content", Message, SDL_arraysize(Buttons), Buttons, NULL };
        int Choice = Quit;
        if (!SDL_ShowMessageBox(&Box, &Choice) || Choice == Quit)
            return false;
        FInstall I = {};
        I.Root = ContentRoot;
        bool Ok;
        SetupQuit = false;
        if (Choice == Reduced)
        {
            I.Src = UT_ASSETS_URL;
            Ok = DownloadAndUnpack(I);
            if (Ok && !I.Files)
                Ok = I.Fail("No Animations, Maps, Music, Sounds, StaticMeshes or Textures folders were found in %s.");
        }
        else if (Choice == Full)
        {
            I.Src = UT_FULL_URL;
            Ok = InstallFull(I);
        }
        else
            Ok = InstallFromDocuments(Root, I);
        CloseSetupScreen();
        if (Ok && HasMaps(ContentRoot))
        {
            // Written last, so an interrupted install asks again on the next launch.
            SDL_SaveFile(StampPath, I.Src ? I.Src : "Documents", SDL_strlen(I.Src ? I.Src : "Documents"));
            return true;
        }
        SDL_snprintf(Message, sizeof(Message), "%s\n\nChoose where the game content comes from.",
            I.Err[0] ? I.Err : "The game content is incomplete, its Maps folder holds no maps.");
    }
}

/*-----------------------------------------------------------------------------
	Startup.
-----------------------------------------------------------------------------*/

bool IOSSetup()
{
    SDL_AddEventWatch(WatchApp, NULL);
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
        return false;
    // Documents, so the inis, the log and the content can be reached with the Files app (UIFileSharingEnabled).
    char Root[1024];
    const char* Documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    if (!Documents)
        return IOSFail("Couldn't find the app's Documents folder: %s", SDL_GetError());
    SDL_strlcpy(Root, Documents, sizeof(Root));
    size_t Len = SDL_strlen(Root);
    if (Len > 1 && Root[Len - 1] == '/')
        Root[Len - 1] = 0;
    bool Ok = InstallBundleFiles(Root) && InstallContent(Root);
    CloseSetupScreen();
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    if (!Ok)
        return false;
    // Caches is not backed up, and the system may empty it when space runs low, which is what the download cache is for.
    if (const char* Home = getenv("HOME"))
    {
        char Cache[1024];
        SDL_snprintf(Cache, sizeof(Cache), "%s/Library/Caches", Home);
        setenv("XDG_CACHE_HOME", Cache, 1);
    }
    // The engine runs from System/ like on the desktop.
    char SystemDir[1024];
    SDL_snprintf(SystemDir, sizeof(SystemDir), "%s/UT2004/System", Root);
    if (chdir(SystemDir) != 0)
        return IOSFail("Couldn't open %s.", SystemDir);
    StartWatchdog();
    return true;
}
