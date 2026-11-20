// version.dll プロキシ
// SDL2.dll が VERSION.dll をインポートしており、version.dll は KnownDLLs に含まれないため、
// ゲームフォルダに置いたこの DLL が先に読み込まれる。本物の関数はシステムの version.dll へ中継する。
#pragma once

static FARPROC g_vp[15];
static const char* const kVerNames[15] = {
    "GetFileVersionInfoA", "GetFileVersionInfoByHandle", "GetFileVersionInfoExA", "GetFileVersionInfoExW",
    "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExA", "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",
    "GetFileVersionInfoW", "VerFindFileA", "VerFindFileW", "VerInstallFileA", "VerInstallFileW",
    "VerQueryValueA", "VerQueryValueW",
};

static void load_real_version() {
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\version.dll");
    HMODULE h = LoadLibraryW(path);
    for (int i = 0; i < 15; i++) g_vp[i] = h ? GetProcAddress(h, kVerNames[i]) : nullptr;
}

#define VP(i, name) \
    extern "C" __declspec(naked) void vp_##name() { __asm jmp dword ptr [g_vp + i * 4] } \
    __pragma(comment(linker, "/export:" #name "=_vp_" #name))

VP(0, GetFileVersionInfoA)
VP(1, GetFileVersionInfoByHandle)
VP(2, GetFileVersionInfoExA)
VP(3, GetFileVersionInfoExW)
VP(4, GetFileVersionInfoSizeA)
VP(5, GetFileVersionInfoSizeExA)
VP(6, GetFileVersionInfoSizeExW)
VP(7, GetFileVersionInfoSizeW)
VP(8, GetFileVersionInfoW)
VP(9, VerFindFileA)
VP(10, VerFindFileW)
VP(11, VerInstallFileA)
VP(12, VerInstallFileW)
VP(13, VerQueryValueA)
VP(14, VerQueryValueW)

#pragma comment(linker, "/export:VerLanguageNameA=KERNEL32.VerLanguageNameA")
#pragma comment(linker, "/export:VerLanguageNameW=KERNEL32.VerLanguageNameW")
