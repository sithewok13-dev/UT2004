// The .tar.xz, .tar and .zip reader of the Android and iOS launchers.
#include <time.h>  // dodging a macro clash with Unix C runtime headers...

#include "SDLLaunchPrivate.h"
#include "SDLLaunchContent.h"

#pragma pack(push,8)
#include <lzma.h>
#pragma pack(pop)
#include <errno.h>
#include <unistd.h>

// Reads a stream with or without xz compression, the first bytes stay buffered to tell the format.
struct FStreamReader
{
    int Fd;
    FInstall& I;
    bool Xz = false, Eof = false;
    lzma_stream Strm = LZMA_STREAM_INIT;
    lzma_ret Ret = LZMA_OK;
    uint8_t In[1 << 16];
    uint8_t* Next = In;
    size_t Avail = 0;

    FStreamReader(int InFd, FInstall& InI) : Fd(InFd), I(InI) {}
    ~FStreamReader() { lzma_end(&Strm); }

    bool Fill()
    {
        if (SetupQuit)
            return false;
        ssize_t Got;
        do
            Got = read(Fd, In + Avail, sizeof(In) - Avail);
        while (Got < 0 && errno == EINTR);
        if (Got <= 0)
        {
            Eof = true;
            return false;
        }
        Avail += Got;
        I.Progress(Got);
        return true;
    }
    void Peek(size_t N)
    {
        while (Avail < N && Fill()) {}
    }
    // Exactly N bytes, false at the end of the stream or on a decoder error.
    bool Read(void* Out, size_t N)
    {
        uint8_t* P = (uint8_t*)Out;
        while (N)
        {
            if (!Avail && !Eof)
            {
                Next = In;
                Fill();
            }
            if (!Xz)
            {
                if (!Avail)
                    return false;
                size_t Count = SDL_min(N, Avail);
                SDL_memcpy(P, Next, Count);
                Next += Count;
                Avail -= Count;
                P += Count;
                N -= Count;
                continue;
            }
            if (Ret != LZMA_OK)
                return false;
            Strm.next_in = Next;
            Strm.avail_in = Avail;
            Strm.next_out = P;
            Strm.avail_out = N;
            Ret = lzma_code(&Strm, Eof ? LZMA_FINISH : LZMA_RUN);
            Next += Avail - Strm.avail_in;
            Avail = Strm.avail_in;
            P += N - Strm.avail_out;
            N = Strm.avail_out;
            if (Ret != LZMA_OK && (Ret != LZMA_STREAM_END || N))
                return false;
        }
        return true;
    }
    bool Fail()
    {
        return I.Fail(Ret == LZMA_MEM_ERROR ? "Not enough memory to unpack %s." : "%s is damaged or incomplete.");
    }
};

static Sint64 TarNumber(const char* Field, INT Len)
{
    // GNU tar stores sizes over 8 GB in base 256.
    if (Field[0] & 0x80)
    {
        Sint64 Value = 0;
        for (INT i = 1; i < Len; i++)
            Value = Value << 8 | (uint8_t)Field[i];
        return Value;
    }
    Sint64 Value = 0;
    for (INT i = 0; i < Len && Field[i] >= '0' && Field[i] <= '7'; i++)
        Value = Value * 8 + Field[i] - '0';
    return Value;
}

static bool TarHeaderValid(const char* Hdr)
{
    unsigned Sum = 0;
    for (INT i = 0; i < 512; i++)
        Sum += i >= 148 && i < 156 ? ' ' : (uint8_t)Hdr[i];
    return Sum == TarNumber(Hdr + 148, 8);
}

// Pax records are "<length> <key>=<value>\n", only the path matters.
static void PaxPath(const char* Data, size_t Len, char* Name, size_t NameSize)
{
    for (const char* P = Data; P < Data + Len; )
    {
        char* Key;
        long RecLen = SDL_strtol(P, &Key, 10);
        if (RecLen <= 0 || *Key != ' ' || P + RecLen > Data + Len)
            return;
        Key++;
        if (SDL_strncmp(Key, "path=", 5) == 0)
            SDL_strlcpy(Name, Key + 5, SDL_min(NameSize, (size_t)(P + RecLen - (Key + 5))));
        P += RecLen;
    }
}

static bool Untar(FStreamReader& R, FInstall& I)
{
    static uint8_t Buf[1 << 18];
    char Hdr[512], LongName[1024] = "";
    while (R.Read(Hdr, sizeof(Hdr)))
    {
        if (!Hdr[0])
            return true;
        if (!TarHeaderValid(Hdr))
            return R.Fail();
        Sint64 Size = TarNumber(Hdr + 124, 12);
        char Type = Hdr[156];
        char Name[1024];
        if (LongName[0])
            SDL_strlcpy(Name, LongName, sizeof(Name));
        // Only POSIX ustar has a name prefix, old GNU headers keep times there.
        else if (SDL_memcmp(Hdr + 257, "ustar", 6) == 0 && Hdr[345])
            SDL_snprintf(Name, sizeof(Name), "%.155s/%.100s", Hdr + 345, Hdr);
        else
            SDL_snprintf(Name, sizeof(Name), "%.100s", Hdr);
        LongName[0] = 0;

        char Dest[1024];
        SDL_IOStream* Out = NULL;
        if ((Type == '0' || Type == 0) && ContentPath(I, Name, Dest, sizeof(Dest)))
        {
            Out = CreateContentFile(Dest);
            if (!Out)
                return I.Fail("Couldn't write the content of %s. Is the device out of storage?");
        }
        // Long names come in an entry of their own just before the file.
        bool Meta = Type == 'L' || Type == 'x';
        char MetaData[sizeof(LongName)];
        size_t MetaLen = 0;
        bool ReadOk = true, WriteOk = true;
        for (Sint64 Left = Size; ReadOk && WriteOk && Left > 0; Left -= (Sint64)sizeof(Buf))
        {
            size_t N = (size_t)SDL_min(Left, (Sint64)sizeof(Buf));
            ReadOk = R.Read(Buf, N);
            if (ReadOk && Out)
                WriteOk = SDL_WriteIO(Out, Buf, N) == N;
            if (ReadOk && Meta && !MetaLen)
            {
                MetaLen = SDL_min(N, sizeof(MetaData) - 1);
                SDL_memcpy(MetaData, Buf, MetaLen);
                MetaData[MetaLen] = 0;
            }
        }
        if (Out && !SDL_CloseIO(Out))
            WriteOk = false;
        if (!WriteOk)
            return I.Fail("Couldn't write the content of %s. Is the device out of storage?");
        if (!ReadOk || !R.Read(Buf, (512 - Size % 512) % 512))
            return R.Fail();
        I.Files += Out != NULL;
        if (Type == 'L')
            SDL_strlcpy(LongName, MetaData, sizeof(LongName));
        else if (Type == 'x')
            PaxPath(MetaData, MetaLen, LongName, sizeof(LongName));
    }
    return R.Fail();
}

bool UnpackStream(int Fd, FInstall& I)
{
    FStreamReader R(Fd, I);
    R.Peek(512);
    static const uint8_t XzMagic[] = { 0xFD, '7', 'z', 'X', 'Z', 0 };
    if (R.Avail >= 6 && SDL_memcmp(R.In, XzMagic, 6) == 0)
    {
        R.Xz = true;
        if (lzma_stream_decoder(&R.Strm, UINT64_MAX, 0) != LZMA_OK)
            return I.Fail("Not enough memory to unpack %s.");
        return Untar(R, I);
    }
    if (R.Avail >= 4 && SDL_memcmp(R.In, "PK\3\4", 4) == 0)
    {
        // Unzip counts what it reads itself.
        I.Done = I.Drawn = 0;
        // SDL opens content:// URIs through the ContentResolver, a download has nothing to seek in.
        bool Local = SDL_strncmp(I.Src, "content:", 8) == 0 || I.Src[0] == '/';
        SDL_IOStream* Zip = Local ? SDL_IOFromFile(I.Src, "rb") : NULL;
        if (!Zip)
            return I.Fail("%s can't be read as a local file, which zip archives need.");
        bool Ok = Unzip(Zip, I);
        SDL_CloseIO(Zip);
        return Ok;
    }
    if (R.Avail >= 512 && TarHeaderValid((const char*)R.In))
        return Untar(R, I);
    return I.Fail("%s is not a .tar.xz, .tar or .zip archive.");
}
