#include <time.h>  // dodging a macro clash with Unix C runtime headers...

#include "SDLLaunchPrivate.h"
#include "SDLLaunchContent.h"

#pragma pack(push,8)
#include <lzma.h>
#include <jni.h>
#pragma pack(pop)
#include <errno.h>
#include <unistd.h>

// Android has one window, so SDLDrv takes this one over and the logo stays up until the game draws.
// It gets the GL ES 3 context OpenGLDrv asks for, so the window's EGL config suits that one too.
void InitSplash(const TCHAR *filename)
{
    SDL_Surface *bmp = filename ? SDL_LoadBMP(appToAnsi(filename)) : NULL;
    if (bmp == NULL || !SDL_Init(SDL_INIT_VIDEO))
    {
        SDL_DestroySurface(bmp);
        return;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    // SDLDrv destroys the renderer when it adopts the window.
    SDL_Window *window = NULL;
    SDL_Renderer *renderer = NULL;
    if (SDL_CreateWindowAndRenderer("UT2004", 0, 0, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN, &window, &renderer))
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
    }
    SDL_DestroySurface(bmp);
}

bool AndroidFail(const char* Fmt, const char* Arg)
{
    char Msg[1024];
    SDL_snprintf(Msg, sizeof(Msg), Fmt, Arg);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "UT2004", Msg, NULL);
    return false;
}

// Decodes the APK asset <Rel>.xz into Dest.
static bool ExtractAsset(const char* Rel, const char* Dest, lzma_stream& Strm)
{
    static uint8_t InBuf[1 << 16], OutBuf[1 << 18];
    char Path[1024];
    SDL_snprintf(Path, sizeof(Path), "%s.xz", Rel);
    // A relative path is looked up in the APK assets.
    SDL_IOStream* In = SDL_IOFromFile(Path, "rb");
    if (!In)
        return false;
    SDL_strlcpy(Path, Dest, sizeof(Path));
    *SDL_strrchr(Path, '/') = 0;
    SDL_CreateDirectory(Path);
    SDL_IOStream* Out = SDL_IOFromFile(Dest, "wb");
    bool Ok = Out && lzma_stream_decoder(&Strm, UINT64_MAX, 0) == LZMA_OK;
    Strm.avail_in = 0;
    lzma_ret Ret = LZMA_OK;
    while (Ok && Ret != LZMA_STREAM_END)
    {
        if (Strm.avail_in == 0)
        {
            Strm.next_in = InBuf;
            Strm.avail_in = SDL_ReadIO(In, InBuf, sizeof(InBuf));
        }
        Strm.next_out = OutBuf;
        Strm.avail_out = sizeof(OutBuf);
        Ret = lzma_code(&Strm, Strm.avail_in ? LZMA_RUN : LZMA_FINISH);
        size_t Got = sizeof(OutBuf) - Strm.avail_out;
        Ok = (Ret == LZMA_OK || Ret == LZMA_STREAM_END) && SDL_WriteIO(Out, OutBuf, Got) == Got;
    }
    SDL_CloseIO(In);
    if (Out && !SDL_CloseIO(Out))
        Ok = false;
    return Ok;
}

// The engine reads its files with fopen, which can't see into the APK, so they are unpacked once per APK build.
bool ExtractAssets(const char* Root)
{
    char* Manifest = (char*)SDL_LoadFile("manifest.txt", NULL);
    char* Rel = Manifest ? SDL_strchr(Manifest, '\n') : NULL;
    if (!Rel)
        return AndroidFail("The APK has no %s, it was built without assets.", "manifest.txt");
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

    lzma_stream Strm = LZMA_STREAM_INIT;
    Ok = true;
    for (INT Done = 0; Ok && !SetupQuit && *Rel; Done++)
    {
        char* End = SDL_strchr(Rel, '\n');
        *End = 0;
        DrawProgress("Unpacking game files", (float)Done / Total);
        char Dest[1024];
        SDL_snprintf(Dest, sizeof(Dest), "%s/%s", Root, Rel);
        Ok = ExtractAsset(Rel, Dest, Strm);
        if (!Ok)
            AndroidFail("Couldn't unpack %s. Is the device out of storage?", Rel);
        Rel = End + 1;
    }
    lzma_end(&Strm);

    // Written last, so an interrupted unpack starts over on the next launch.
    Ok = Ok && !SetupQuit;
    if (Ok)
        SDL_SaveFile(StampPath, Manifest, SDL_strlen(Manifest));
    SDL_free(Manifest);
    return Ok;
}

// Turns a pending Java exception into the error message.
static bool JavaFailed(JNIEnv* Env, FInstall& I)
{
    jthrowable E = Env->ExceptionOccurred();
    if (!E)
        return false;
    Env->ExceptionClear();
    jclass Throwable = Env->FindClass("java/lang/Throwable");
    jstring Msg = (jstring)Env->CallObjectMethod(E, Env->GetMethodID(Throwable, "toString", "()Ljava/lang/String;"));
    const char* Chars = Msg ? Env->GetStringUTFChars(Msg, NULL) : NULL;
    SDL_snprintf(I.Err, sizeof(I.Err), "Couldn't get the game content from %s:\n%s", I.Src, Chars ? Chars : "");
    if (Chars)
        Env->ReleaseStringUTFChars(Msg, Chars);
    Env->DeleteLocalRef(Msg);
    Env->DeleteLocalRef(Throwable);
    Env->DeleteLocalRef(E);
    return true;
}

static bool CopyFile(const char* Uri, const char* Dest, FInstall& I)
{
    static uint8_t Buf[1 << 18];
    // SDL opens content:// URIs through the ContentResolver.
    SDL_IOStream* In = SDL_IOFromFile(Uri, "rb");
    if (!In)
        return I.Fail("Couldn't open a file in %s.");
    SDL_IOStream* Out = CreateContentFile(Dest);
    bool WriteOk = Out != NULL;
    size_t Got;
    while (WriteOk && (Got = SDL_ReadIO(In, Buf, sizeof(Buf))) > 0)
        WriteOk = SDL_WriteIO(Out, Buf, Got) == Got;
    bool ReadOk = SDL_GetIOStatus(In) != SDL_IO_STATUS_ERROR;
    SDL_CloseIO(In);
    if (Out && !SDL_CloseIO(Out))
        WriteOk = false;
    if (!WriteOk)
        return I.Fail("Couldn't write the content of %s. Is the device out of storage?");
    return ReadOk || I.Fail("Couldn't read a file in %s.");
}

static bool CopyTree(JNIEnv* Env, jobjectArray Tree, FInstall& I)
{
    jsize Total = Env->GetArrayLength(Tree) / 2;
    bool Ok = true;
    for (jsize i = 0; Ok && !SetupQuit && i < Total; i++)
    {
        jstring JUri = (jstring)Env->GetObjectArrayElement(Tree, 2 * i);
        jstring JRel = (jstring)Env->GetObjectArrayElement(Tree, 2 * i + 1);
        const char* Uri = Env->GetStringUTFChars(JUri, NULL);
        const char* Rel = Env->GetStringUTFChars(JRel, NULL);
        char Dest[1024], Label[128];
        SDL_snprintf(Label, sizeof(Label), "Copying assets  %d / %d files", (int)i, (int)Total);
        DrawProgress(Label, (float)i / Total);
        if (ContentPath(I, Rel, Dest, sizeof(Dest)))
        {
            Ok = CopyFile(Uri, Dest, I);
            I.Files++;
        }
        Env->ReleaseStringUTFChars(JUri, Uri);
        Env->ReleaseStringUTFChars(JRel, Rel);
        Env->DeleteLocalRef(JUri);
        Env->DeleteLocalRef(JRel);
    }
    return Ok;
}

// The download is unpacked while it arrives, as the device may not have room for the archive too.
static bool DigestMatches(JNIEnv* Env, jclass Activity, int Fd)
{
    static uint8_t Buf[1 << 16];
    // The digest covers every byte of the download, which may go on past the end of the archive.
    for (errno = 0; !SetupQuit && (read(Fd, Buf, sizeof(Buf)) > 0 || errno == EINTR); )
        errno = 0;
    jstring JDigest = (jstring)Env->GetStaticObjectField(Activity, Env->GetStaticFieldID(Activity, "assetDigest", "Ljava/lang/String;"));
    const char* Digest = JDigest ? Env->GetStringUTFChars(JDigest, NULL) : NULL;
    bool Ok = !SetupQuit && Digest && SDL_strcasecmp(Digest, UT_ASSETS_SHA256) == 0;
    if (Digest)
        Env->ReleaseStringUTFChars(JDigest, Digest);
    Env->DeleteLocalRef(JDigest);
    return Ok;
}

static bool TryInstall(JNIEnv* Env, jclass Activity, FInstall& I)
{
    // The full game is too large to unpack while it arrives, and its download has to survive a lost connection.
    if (SDL_strcmp(I.Src, UT_FULL_URL) == 0)
        return InstallFull(I);
    jstring JSrc = Env->NewStringUTF(I.Src);
    jmethodID ListTree = Env->GetStaticMethodID(Activity, "listAssetTree", "(Ljava/lang/String;)[Ljava/lang/String;");
    jobjectArray Tree = (jobjectArray)Env->CallStaticObjectMethod(Activity, ListTree, JSrc);
    bool Ok = !JavaFailed(Env, I);
    if (Ok && Tree)
        Ok = CopyTree(Env, Tree, I);
    else if (Ok)
    {
        jmethodID Open = Env->GetStaticMethodID(Activity, "openAssetStream", "(Ljava/lang/String;)[J");
        I.Verb = SDL_strncmp(I.Src, "content:", 8) ? "Downloading assets" : "Unpacking assets";
        I.Draw();
        jlongArray Stream = (jlongArray)Env->CallStaticObjectMethod(Activity, Open, JSrc);
        Ok = !JavaFailed(Env, I);
        if (Ok)
        {
            jlong FdSize[2];
            Env->GetLongArrayRegion(Stream, 0, 2, FdSize);
            Env->DeleteLocalRef(Stream);
            I.Size = FdSize[1];
            // Only the built-in download has a known digest, anything else is the player's own choice.
            bool Checked = UT_ASSETS_SHA256[0] && SDL_strcmp(I.Src, UT_ASSETS_URL) == 0;
            std::vector<std::string> Created;
            CreatedFiles = Checked ? &Created : NULL;
            Ok = UnpackStream((int)FdSize[0], I);
            CreatedFiles = NULL;
            if (Checked && !(Ok && DigestMatches(Env, Activity, (int)FdSize[0])))
            {
                // Only what this download wrote goes, the folders may hold files the player put there.
                for (const std::string& Path : Created)
                    SDL_RemovePath(Path.c_str());
                if (Ok && !SetupQuit)
                    Ok = I.Fail("%s is not the published game content, its SHA-256 does not match.");
            }
            close((int)FdSize[0]);
        }
    }
    Env->DeleteLocalRef(Tree);
    Env->DeleteLocalRef(JSrc);
    if (Ok && SetupQuit)
        Ok = I.Fail("Setup was closed.");
    if (Ok && !I.Files)
        Ok = I.Fail("No Animations, Maps, Music, Sounds, StaticMeshes or Textures folders were found in %s.");
    return Ok;
}

static bool JavaSource(JNIEnv* Env, jclass Activity, const char* Method, const char* First, const char* Second, char* Src, size_t SrcSize)
{
    jmethodID Ask = Env->GetStaticMethodID(Activity, Method, "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    jstring JFirst = Env->NewStringUTF(First), JSecond = Env->NewStringUTF(Second);
    jstring JSrc = (jstring)Env->CallStaticObjectMethod(Activity, Ask, JFirst, JSecond);
    Env->ExceptionClear();
    Env->DeleteLocalRef(JFirst);
    Env->DeleteLocalRef(JSecond);
    if (!JSrc)
        return false;
    const char* Chars = Env->GetStringUTFChars(JSrc, NULL);
    SDL_strlcpy(Src, Chars, SrcSize);
    Env->ReleaseStringUTFChars(JSrc, Chars);
    Env->DeleteLocalRef(JSrc);
    return true;
}

// The content is too large for the APK, so the player picks on first launch where it comes from.
bool InstallContent(const char* Root)
{
    char StampPath[1024];
    SDL_snprintf(StampPath, sizeof(StampPath), "%s/content.stamp", Root);
    if (SDL_GetPathInfo(StampPath, NULL))
        return true;

    JNIEnv* Env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jobject ActivityObj = (jobject)SDL_GetAndroidActivity();
    jclass Activity = Env->GetObjectClass(ActivityObj);
    Env->DeleteLocalRef(ActivityObj);

    char Src[1024], Url[1024] = UT_ASSETS_URL, ContentRoot[1024];
    SDL_snprintf(ContentRoot, sizeof(ContentRoot), "%s/UT2004", Root);
    bool Ok = false;
    for (bool Asked = JavaSource(Env, Activity, "askContentChoice", UT_ASSETS_URL, UT_FULL_URL, Src, sizeof(Src)); Asked; )
    {
        FInstall I = {};
        I.Root = ContentRoot;
        I.Src = Src;
        if (TryInstall(Env, Activity, I))
        {
            // Written last, so an interrupted install asks again on the next launch.
            SDL_SaveFile(StampPath, Src, SDL_strlen(Src));
            Ok = true;
            break;
        }
        if (SetupQuit)
            break;
        if (SDL_strncmp(Src, "content:", 8) != 0)
            SDL_strlcpy(Url, Src, sizeof(Url));
        // A picker covering the activity destroys the window surface, which must not be current then.
        CloseSetupScreen();
        Asked = JavaSource(Env, Activity, "askAssetSource", I.Err, Url, Src, sizeof(Src));
    }
    Env->DeleteLocalRef(Activity);
    return Ok;
}

void StartServerService(const TCHAR* Cmd)
{
    JNIEnv* Env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jobject ActivityObj = (jobject)SDL_GetAndroidActivity();
    jclass Activity = Env->GetObjectClass(ActivityObj);
    Env->DeleteLocalRef(ActivityObj);
    jchar Chars[1024];
    jsize Len = 0;
    while (Cmd[Len] && Len < (jsize)ARRAY_COUNT(Chars))
        Chars[Len] = (jchar)Cmd[Len], Len++;
    jstring JCmd = Env->NewString(Chars, Len);
    Env->CallStaticVoidMethod(Activity, Env->GetStaticMethodID(Activity, "startServer", "(Ljava/lang/String;)V"), JCmd);
    Env->ExceptionClear();
    Env->DeleteLocalRef(JCmd);
    Env->DeleteLocalRef(Activity);
}

// ServerService has no activity, so the folders SDL finds for the game come from Java.
extern "C" JNIEXPORT jint JNICALL Java_org_ut2004_game_ServerService_runServer( JNIEnv* Env, jclass, jstring Dir, jstring Cache, jstring Cmd )
{
	const char* Chars = Env->GetStringUTFChars( Cache, NULL );
	setenv( "XDG_CACHE_HOME", Chars, 1 );
	Env->ReleaseStringUTFChars( Cache, Chars );
	Chars = Env->GetStringUTFChars( Dir, NULL );
	int Failed = chdir( Chars );
	Env->ReleaseStringUTFChars( Dir, Chars );
	if( Failed )
		return 1;
	TCHAR CmdLine[1024];
	jchar Buf[ARRAY_COUNT(CmdLine)];
	jsize Len = Min<jsize>( Env->GetStringLength( Cmd ), ARRAY_COUNT(CmdLine) - 1 );
	Env->GetStringRegion( Cmd, 0, Len, Buf );
	for( jsize i=0; i<Len; i++ )
		CmdLine[i] = Buf[i];
	CmdLine[Len] = 0;
	return RunEngine( CmdLine );
}

extern "C" JNIEXPORT void JNICALL Java_org_ut2004_game_ServerService_stopServer( JNIEnv*, jclass )
{
	GIsRequestingExit = 1;
}
