#include "SDLLaunchContent.h"

#pragma pack(push,8)
#include <zlib.h>
#pragma pack(pop)

static SDL_Window* SetupWindow = NULL;
static SDL_Renderer* SetupRenderer = NULL;
bool SetupQuit = false;

#ifdef __ANDROID__
static const int SetupWidth = 0, SetupHeight = 0, SetupTextRows = 270;
static const SDL_WindowFlags SetupFlags = SDL_WINDOW_FULLSCREEN;
#elif defined(SDL_PLATFORM_IOS)
static const int SetupWidth = 0, SetupHeight = 0, SetupTextRows = 270;
static const SDL_WindowFlags SetupFlags = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#else
static const int SetupWidth = 800, SetupHeight = 180, SetupTextRows = 90;
static const SDL_WindowFlags SetupFlags = 0;
#endif

void PumpSetup()
{
    SDL_PumpEvents();
    SetupQuit |= SDL_HasEvent(SDL_EVENT_QUIT);
}

// Shared by the unpack and install steps, so the screen doesn't flicker between them.
void DrawProgress(const char* Label, float Fraction)
{
    if (!SetupWindow && SDL_Init(SDL_INIT_VIDEO)
        && !SDL_CreateWindowAndRenderer("UT2004", SetupWidth, SetupHeight, SetupFlags, &SetupWindow, &SetupRenderer))
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    PumpSetup();
    if (!SetupRenderer)
        return;
    int W, H;
    SDL_GetCurrentRenderOutputSize(SetupRenderer, &W, &H);
    SDL_FRect Back = { W * 0.1f, H * 0.48f, W * 0.8f, H * 0.04f };
    SDL_FRect Bar = Back;
    Bar.w *= SDL_clamp(Fraction, 0.0f, 1.0f);
    SDL_SetRenderDrawColor(SetupRenderer, 0, 0, 0, 255);
    SDL_RenderClear(SetupRenderer);
    SDL_SetRenderDrawColor(SetupRenderer, 60, 60, 60, 255);
    SDL_RenderFillRect(SetupRenderer, &Back);
    SDL_SetRenderDrawColor(SetupRenderer, 40, 120, 230, 255);
    SDL_RenderFillRect(SetupRenderer, &Bar);
    // The debug font is 8 pixels, whole multiples keep it sharp.
    float Scale = SDL_max(1.0f, SDL_floorf((float)H / SetupTextRows));
    float Char = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
    SDL_SetRenderScale(SetupRenderer, Scale, Scale);
    SDL_SetRenderDrawColor(SetupRenderer, 220, 220, 220, 255);
    SDL_RenderDebugText(SetupRenderer, (W / Scale - SDL_strlen(Label) * Char) / 2, Back.y / Scale - 2 * Char, Label);
    SDL_SetRenderScale(SetupRenderer, 1.0f, 1.0f);
    SDL_RenderPresent(SetupRenderer);
}

void CloseSetupScreen()
{
    if (!SetupWindow)
        return;
    SDL_DestroyRenderer(SetupRenderer);
    SDL_DestroyWindow(SetupWindow);
    SetupRenderer = NULL;
    SetupWindow = NULL;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void FInstall::Draw()
{
    char Label[128];
    if (Size > 0)
        SDL_snprintf(Label, sizeof(Label), "%s  %d / %d MB", Verb, (int)(Done >> 20), (int)(Size >> 20));
    else
        SDL_snprintf(Label, sizeof(Label), "%s  %d MB", Verb, (int)(Done >> 20));
    DrawProgress(Label, Size > 0 ? (float)Done / Size : 0.0f);
}

void FInstall::Progress(Sint64 Bytes)
{
    Done += Bytes;
    if (Done - Drawn >= (1 << 20))
    {
        Drawn = Done;
        Draw();
    }
}

bool FInstall::Fail(const char* Fmt)
{
    SDL_snprintf(Err, sizeof(Err), Fmt, Src);
    return false;
}

static const char* ContentDirs[] = { "Animations", "Maps", "Music", "Sounds", "StaticMeshes", "Textures" };

// Archives and folders come from anywhere, so only files inside the content folders are written.
// The folders may sit one level down, as in an archive of a whole game folder.
bool ContentPath(const FInstall& I, const char* Rel, char* Dest, size_t DestSize)
{
    if (SDL_strstr(Rel, ".."))
        return false;
    for (int Level = 0; Level < 2 && Rel; Level++)
    {
        for (const char* Dir : ContentDirs)
        {
            size_t Len = SDL_strlen(Dir);
            if (SDL_strncasecmp(Rel, Dir, Len) == 0 && Rel[Len] == '/' && Rel[Len + 1])
            {
                // The engine looks the folders up by these names.
                SDL_snprintf(Dest, DestSize, "%s/%s%s", I.Root, Dir, Rel + Len);
                return true;
            }
        }
        Rel = SDL_strchr(Rel, '/');
        if (Rel)
            Rel++;
    }
    return false;
}

std::vector<std::string>* CreatedFiles;

SDL_IOStream* CreateContentFile(const char* Dest)
{
    if (CreatedFiles)
        CreatedFiles->push_back(Dest);
    char Dir[1024];
    SDL_strlcpy(Dir, Dest, sizeof(Dir));
    *SDL_strrchr(Dir, '/') = 0;
    SDL_CreateDirectory(Dir);
    return SDL_IOFromFile(Dest, "wb");
}

static uint32_t Le16(const uint8_t* P) { return P[0] | P[1] << 8; }
static uint32_t Le32(const uint8_t* P) { return Le16(P) | (uint32_t)Le16(P + 2) << 16; }
static uint64_t Le64(const uint8_t* P) { return Le32(P) | (uint64_t)Le32(P + 4) << 32; }

static bool ReadAt(SDL_IOStream* Zip, void* Out, size_t N, Sint64 Offset)
{
    return !SetupQuit && SDL_SeekIO(Zip, Offset, SDL_IO_SEEK_SET) == Offset && SDL_ReadIO(Zip, Out, N) == N;
}

// Writes one zip entry, stored or deflated, to Dest.
static bool UnzipEntry(SDL_IOStream* Zip, FInstall& I, Sint64 Offset, Sint64 CSize, int Method, const char* Dest)
{
    static uint8_t In[1 << 16], Out[1 << 18];
    SDL_IOStream* File = CreateContentFile(Dest);
    if (!File)
        return I.Fail("Couldn't write the content of %s. Is the device out of storage?");
    z_stream Z = {};
    bool Ok = Method == 0 || inflateInit2(&Z, -MAX_WBITS) == Z_OK;
    bool Inflating = Ok && Method == 8;
    bool WriteOk = true;
    for (Sint64 Left = CSize; Ok && WriteOk; )
    {
        if (!Z.avail_in)
        {
            if (!Left)
            {
                // Stored data ends with its size, a deflate stream has to reach its end marker first.
                Ok = Method == 0;
                break;
            }
            size_t N = (size_t)SDL_min(Left, (Sint64)sizeof(In));
            Ok = ReadAt(Zip, In, N, Offset);
            Offset += N;
            Left -= N;
            I.Progress(N);
            Z.next_in = In;
            Z.avail_in = (uInt)N;
            continue;
        }
        if (Method == 0)
        {
            WriteOk = SDL_WriteIO(File, In, Z.avail_in) == Z.avail_in;
            Z.avail_in = 0;
            continue;
        }
        Z.next_out = Out;
        Z.avail_out = sizeof(Out);
        int Ret = inflate(&Z, Z_NO_FLUSH);
        size_t Got = sizeof(Out) - Z.avail_out;
        Ok = Ret == Z_OK || Ret == Z_STREAM_END;
        WriteOk = SDL_WriteIO(File, Out, Got) == Got;
        if (Ret == Z_STREAM_END)
            break;
    }
    if (Inflating)
        inflateEnd(&Z);
    if (!SDL_CloseIO(File))
        WriteOk = false;
    if (!WriteOk)
        return I.Fail("Couldn't write the content of %s. Is the device out of storage?");
    return Ok || I.Fail("%s is damaged.");
}

bool Unzip(SDL_IOStream* Zip, FInstall& I)
{
    Sint64 Size = SDL_GetIOSize(Zip);
    if (Size < 0)
        return I.Fail("%s can't be read as a local file, which zip archives need.");
    static uint8_t Tail[22 + 65535];
    Sint64 TailLen = SDL_min(Size, (Sint64)sizeof(Tail));
    if (!ReadAt(Zip, Tail, (size_t)TailLen, Size - TailLen))
        return I.Fail("Couldn't read %s.");
    Sint64 End = TailLen - 22;
    while (End >= 0 && Le32(Tail + End) != 0x06054b50)
        End--;
    if (End < 0)
        return I.Fail("%s is damaged.");
    uint64_t Count = Le16(Tail + End + 10), DirSize = Le32(Tail + End + 12), DirOffset = Le32(Tail + End + 16);
    uint8_t Zip64[56];
    if (End >= 20 && Le32(Tail + End - 20) == 0x07064b50)
    {
        if (!ReadAt(Zip, Zip64, sizeof(Zip64), Le64(Tail + End - 12)) || Le32(Zip64) != 0x06064b50)
            return I.Fail("%s is damaged.");
        Count = Le64(Zip64 + 32);
        DirSize = Le64(Zip64 + 40);
        DirOffset = Le64(Zip64 + 48);
    }
    if (DirOffset + DirSize > (uint64_t)Size)
        return I.Fail("%s is damaged.");
    uint8_t* Dir = (uint8_t*)SDL_malloc((size_t)DirSize);
    bool Ok = Dir && ReadAt(Zip, Dir, (size_t)DirSize, DirOffset);
    if (!Ok)
        I.Fail("Couldn't read %s.");
    const uint8_t* P = Dir;
    const uint8_t* DirEnd = Dir + DirSize;
    for (uint64_t i = 0; Ok && i < Count; i++)
    {
        if (P + 46 > DirEnd || Le32(P) != 0x02014b50 || P + 46 + Le16(P + 28) + Le16(P + 30) + Le16(P + 32) > DirEnd)
        {
            Ok = I.Fail("%s is damaged.");
            break;
        }
        int Flags = Le16(P + 8), Method = Le16(P + 10), NameLen = Le16(P + 28), ExtraLen = Le16(P + 30);
        uint64_t CSize = Le32(P + 20), USize = Le32(P + 24), Local = Le32(P + 42);
        // Zip64 moves the fields that overflowed into an extra record, in this order.
        const uint8_t* ExtraEnd = P + 46 + NameLen + ExtraLen;
        for (const uint8_t* E = P + 46 + NameLen; E + 4 <= ExtraEnd; E += 4 + Le16(E + 2))
        {
            if (Le16(E) != 0x0001)
                continue;
            const uint8_t* F = E + 4;
            const uint8_t* FEnd = SDL_min(E + 4 + Le16(E + 2), ExtraEnd);
            if (USize == 0xFFFFFFFF && F + 8 <= FEnd) { USize = Le64(F); F += 8; }
            if (CSize == 0xFFFFFFFF && F + 8 <= FEnd) { CSize = Le64(F); F += 8; }
            if (Local == 0xFFFFFFFF && F + 8 <= FEnd) Local = Le64(F);
        }
        char Name[1024];
        SDL_snprintf(Name, sizeof(Name), "%.*s", NameLen, (const char*)P + 46);
        // Some Windows tools write backslashes.
        for (char* C = Name; *C; C++)
            if (*C == '\\')
                *C = '/';
        P = ExtraEnd + Le16(P + 32);

        char Dest[1024];
        uint8_t LocalHdr[30];
        if (!Name[0] || Name[SDL_strlen(Name) - 1] == '/' || !ContentPath(I, Name, Dest, sizeof(Dest)))
            I.Progress(CSize);
        else if (Flags & 1)
            Ok = I.Fail("%s is encrypted.");
        else if (Method != 0 && Method != 8)
            Ok = I.Fail("%s uses a compression other than deflate.");
        else if (!ReadAt(Zip, LocalHdr, sizeof(LocalHdr), Local) || Le32(LocalHdr) != 0x04034b50)
            Ok = I.Fail("%s is damaged.");
        else
        {
            Ok = UnzipEntry(Zip, I, Local + 30 + Le16(LocalHdr + 26) + Le16(LocalHdr + 28), CSize, Method, Dest);
            I.Files++;
        }
    }
    SDL_free(Dir);
    return Ok;
}
