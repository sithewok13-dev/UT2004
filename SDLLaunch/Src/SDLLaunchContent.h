// First run setup shared by the desktop and Android launchers: the progress screen and the zip reader.
// SDLLaunchContent.cpp uses SDL and zlib only, so it also builds on its own for the test.

#pragma once

// SDL3.dll uses default packing, the engine builds with /Zp4.
#pragma pack(push,8)
#include <SDL3/SDL.h>
#include <string>
#include <vector>
#pragma pack(pop)

// The app was closed during setup, every step stops at its next read.
extern bool SetupQuit;

// Keeps the setup window answering and notes when it gets closed.
void PumpSetup();
void DrawProgress(const char* Label, float Fraction);
void CloseSetupScreen();

// One attempt at installing the content folders from a URL, an archive or a folder.
struct FInstall
{
    // The folder that gets Maps, Textures and the other content folders.
    const char* Root;
    const char* Src;
    const char* Verb;
    Sint64 Size, Done, Drawn;
    int Files;
    char Err[1024];

    void Draw();
    void Progress(Sint64 Bytes);
    bool Fail(const char* Fmt);
};

bool ContentPath(const FInstall& I, const char* Rel, char* Dest, size_t DestSize);

// Set while a download with a known digest unpacks, so a wrong one can be taken out again.
extern std::vector<std::string>* CreatedFiles;
SDL_IOStream* CreateContentFile(const char* Dest);

// Zip keeps its index at the end, so the stream has to seek.
bool Unzip(SDL_IOStream* Zip, FInstall& I);

// SDLLaunchTar.cpp, Android and iOS. Unpacks a .tar.xz or .tar read from Fd, or the zip archive at I.Src, into I.Root.
bool UnpackStream(int Fd, FInstall& I);

// HttpFile.cpp. Downloads UT_FULL_URL next to the content folders, resuming an earlier attempt, checks it and unpacks it.
bool InstallFull(FInstall& I);
