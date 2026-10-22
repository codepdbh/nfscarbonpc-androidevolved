/*
 * nfsco_capture.c - record what NFSCO's palmont.dll changes in the game image.
 *
 * The NFSCO bootstrapper maps nfsc.exe at 0x00400000 inside its own process and
 * calls palmont.dll!OnAttach, which patches the game's code in memory (hooks
 * and trampolines). Statically recompiled code never sees such patches, so
 * this tool reproduces only that setup, without running the game: it maps
 * NFSC.exe, binds its imports, loads palmont.dll, calls OnAttach and then
 * writes
 *   patches.txt   every changed byte range of the image (address, before, after)
 *   image.bin     the patched image (SizeOfImage bytes from 0x00400000)
 *   region_<va>.bin  executable private memory created by OnAttach (trampolines)
 *   modules.txt   load addresses of palmont.dll and logging.dll
 * into the output directory. Run it from a copy of the game folder (palmont
 * rewrites NFSCO/*.json and NFSCO/logs).
 *
 * Build (x86, fixed base away from 0x00400000):
 *   cl /O2 nfsco_capture.c /link /FIXED /BASE:0x30000000 /DYNAMICBASE:NO
 * Usage: nfsco_capture <game copy> <output dir> [seconds to wait after OnAttach]
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char* g_before;
static DWORD g_size;
static char g_out[MAX_PATH];

static void fail(const char* what) { fprintf(stderr, "nfsco_capture: %s (error %lu)\n", what, GetLastError()); ExitProcess(1); }

static unsigned char* read_file(const char* path, DWORD* size) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) fail(path);
    *size = GetFileSize(f, NULL);
    unsigned char* data = malloc(*size); DWORD got = 0;
    if (!data || !ReadFile(f, data, *size, &got, NULL) || got != *size) fail("read");
    CloseHandle(f); return data;
}

static void write_file(const char* name, const void* data, DWORD size) {
    char path[MAX_PATH]; snprintf(path, sizeof path, "%s\\%s", g_out, name);
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD put = 0;
    if (f == INVALID_HANDLE_VALUE || !WriteFile(f, data, size, &put, NULL)) fail(path);
    CloseHandle(f);
}

/* Maps the image at its preferred base and binds the imports with the real DLLs. */
static unsigned char* map_game(const char* exe) {
    DWORD size; unsigned char* file = read_file(exe, &size);
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)file;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(file + dos->e_lfanew);
    DWORD base = nt->OptionalHeader.ImageBase; g_size = nt->OptionalHeader.SizeOfImage;
    /* The parent reserved the range before this process initialised (see main). */
    unsigned char* image = VirtualAlloc((void*)base, g_size, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!image) image = VirtualAlloc((void*)base, g_size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (image != (unsigned char*)base) fail("reserve 0x00400000 (another module is there?)");
    memcpy(image, file, nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        memcpy(image + s[i].VirtualAddress, file + s[i].PointerToRawData, min(s[i].SizeOfRawData, s[i].Misc.VirtualSize ? s[i].Misc.VirtualSize : s[i].SizeOfRawData));
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(image + dir.VirtualAddress); d->Name; ++d) {
        const char* dll = (const char*)(image + d->Name);
        HMODULE m = LoadLibraryA(dll);
        if (!m) { fprintf(stderr, "nfsco_capture: %s not found, its imports stay unbound\n", dll); continue; }
        IMAGE_THUNK_DATA32* names = (IMAGE_THUNK_DATA32*)(image + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA32* iat = (IMAGE_THUNK_DATA32*)(image + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            FARPROC p = (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32)
                ? GetProcAddress(m, (LPCSTR)(names->u1.Ordinal & 0xffff))
                : GetProcAddress(m, ((IMAGE_IMPORT_BY_NAME*)(image + names->u1.AddressOfData))->Name);
            iat->u1.Function = (DWORD)p;
        }
    }
    free(file);
    return image;
}

/* Guest layout shared with the Android runtime: palmont.dll at a fixed base
 * (the capture uses a copy rebased there, see rebase_fixed.py) and every
 * executable allocation it makes served page by page from a fixed pool, so the
 * hooks it writes point to the same addresses on both sides. */
#define PALMONT_BASE 0x01060000u
#define PALMONT_SIZE 0x0017D000u
#define POOL_BASE    0x01200000u
#define POOL_SIZE    0x00040000u
#define WSFIX_BASE   0x01300000u   /* ThirteenAG's NFSCarbon.WidescreenFix.asi, rebased */
#define WSFIX_SIZE   0x00040000u
#define LOGGING_BASE 0x01240000u   /* logging.dll, also rebased (guest_modules.inc layout) */
#define LOGGING_SIZE 0x00028000u
static DWORD g_pool_next = POOL_BASE;
static LPVOID WINAPI pool_virtual_alloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
    if (protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) {
        SIZE_T bytes = (size + 0xFFF) & ~(SIZE_T)0xFFF;   /* pages inside the reservation */
        if (g_pool_next + bytes > POOL_BASE + POOL_SIZE) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
        LPVOID block = VirtualAlloc((void*)g_pool_next, size, MEM_COMMIT, protect);
        printf("nfsco_capture: executable allocation %p (hint %p, %lu bytes)\n", block, address, (unsigned long)size);
        if (block) g_pool_next += (DWORD)bytes;
        return block;
    }
    return VirtualAlloc(address, size, type, protect);
}
/* Points palmont's kernel32!VirtualAlloc import at pool_virtual_alloc. */
static void hook_virtual_alloc(HMODULE module) {
    unsigned char* base = (unsigned char*)module;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp((const char*)(base + d->Name), "kernel32.dll")) continue;
        IMAGE_THUNK_DATA32* names = (IMAGE_THUNK_DATA32*)(base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA32* iat = (IMAGE_THUNK_DATA32*)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            if (strcmp(((IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData))->Name, "VirtualAlloc")) continue;
            DWORD old; VirtualProtect(&iat->u1.Function, 4, PAGE_READWRITE, &old);
            iat->u1.Function = (DWORD)pool_virtual_alloc; VirtualProtect(&iat->u1.Function, 4, old, &old);
            return;
        }
    }
    fail("palmont imports no VirtualAlloc");
}

typedef struct { void* base; SIZE_T size; } Region;
static Region g_regions[4096]; static int g_nregions;

static void snapshot_regions(void) {
    MEMORY_BASIC_INFORMATION mbi; unsigned char* p = 0; g_nregions = 0;
    while (VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi && g_nregions < 4096) {
        if (mbi.State == MEM_COMMIT) { g_regions[g_nregions].base = mbi.BaseAddress; g_regions[g_nregions].size = mbi.RegionSize; ++g_nregions; }
        p = (unsigned char*)mbi.BaseAddress + mbi.RegionSize;
        if ((DWORD)p >= 0x7fff0000u) break;
    }
}

static volatile LONG g_dumped;
static DWORD g_wait_ms = 2000;
static void dump(void);
static void dump_once(void) { if (InterlockedExchange(&g_dumped, 1) == 0) { dump(); fflush(stdout); TerminateProcess(GetCurrentProcess(), 0); } }
/* OnAttach may never return (if it went on to start the game): dump anyway. */
static DWORD WINAPI watchdog(LPVOID unused) { (void)unused; Sleep(g_wait_ms + 8000); printf("nfsco_capture: OnAttach did not return, dumping\n"); dump_once(); return 0; }

static int existed(void* base) { for (int i = 0; i < g_nregions; ++i) if (g_regions[i].base == base) return 1; return 0; }

static void dump(void) {
    unsigned char* image = (unsigned char*)0x00400000;
    char line[512]; static char report[1 << 22]; int used = 0;
    unsigned changed = 0, ranges = 0;
    for (DWORD i = 0; i < g_size; ) {
        if (image[i] == g_before[i]) { ++i; continue; }
        DWORD start = i;
        while (i < g_size && (image[i] != g_before[i] || (i + 1 < g_size && image[i + 1] != g_before[i + 1]))) ++i;
        ++ranges; changed += i - start;
        int n = snprintf(line, sizeof line, "%08X %u ", 0x00400000u + start, i - start);
        for (DWORD k = start; k < i && k < start + 32; ++k) n += snprintf(line + n, sizeof line - n, "%02X", g_before[k]);
        n += snprintf(line + n, sizeof line - n, " -> ");
        for (DWORD k = start; k < i && k < start + 32; ++k) n += snprintf(line + n, sizeof line - n, "%02X", image[k]);
        if (image[start] == 0xE9 || image[start] == 0xE8)
            n += snprintf(line + n, sizeof line - n, "  %s %08X", image[start] == 0xE9 ? "jmp" : "call", 0x00400000u + start + 5 + *(LONG*)(image + start + 1));
        n += snprintf(line + n, sizeof line - n, "\n");
        if (used + n < (int)sizeof report) { memcpy(report + used, line, n); used += n; }
    }
    write_file("patches.txt", report, used);
    write_file("image.bin", image, g_size);
    /* New executable private memory: trampolines and generated code. */
    MEMORY_BASIC_INFORMATION mbi; unsigned char* p = 0; int dumped = 0;
    while (VirtualQuery(p, &mbi, sizeof mbi) == sizeof mbi) {
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && !existed(mbi.BaseAddress) &&
            (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) &&
            mbi.BaseAddress != (void*)0x00400000) {
            char name[64]; snprintf(name, sizeof name, "region_%08X.bin", (unsigned)mbi.BaseAddress);
            write_file(name, mbi.BaseAddress, (DWORD)mbi.RegionSize); ++dumped;
        }
        p = (unsigned char*)mbi.BaseAddress + mbi.RegionSize;
        if ((DWORD)p >= 0x7fff0000u) break;
    }
    if (g_pool_next > POOL_BASE) write_file("pool.bin", (void*)POOL_BASE, g_pool_next - POOL_BASE);
    used = snprintf(report, sizeof report, "pool %08X %08X\npalmont.dll %08X\nlogging.dll %08X\nwidescreenfix %08X\n", POOL_BASE, g_pool_next - POOL_BASE,
                    (unsigned)GetModuleHandleA("palmont.dll"), (unsigned)GetModuleHandleA("logging.dll"),
                    (unsigned)GetModuleHandleA("NFSCarbon.WidescreenFix.asi"));
    write_file("modules.txt", report, used);
    printf("nfsco_capture: %u changed bytes in %u ranges, %d new executable regions -> %s\n", changed, ranges, dumped, g_out);
}

/* Windows maps its own data near 0x00400000 while a process starts, so the
 * game range is reserved in a suspended copy of this program before it runs. */
static int run_child(void) {
    char line[4096];
    snprintf(line, sizeof line, "%s --child", GetCommandLineA());
    STARTUPINFOA si = { sizeof si }; PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, line, NULL, NULL, TRUE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) fail("start child");
    if (!VirtualAllocEx(pi.hProcess, (void*)0x00400000, 0x0082C000, MEM_RESERVE, PAGE_NOACCESS)) {
        TerminateProcess(pi.hProcess, 1); fail("reserve 0x00400000 in the child");
    }
    if (!VirtualAllocEx(pi.hProcess, (void*)PALMONT_BASE, PALMONT_SIZE, MEM_RESERVE, PAGE_NOACCESS) ||
        !VirtualAllocEx(pi.hProcess, (void*)POOL_BASE, POOL_SIZE, MEM_RESERVE, PAGE_NOACCESS) ||
        !VirtualAllocEx(pi.hProcess, (void*)LOGGING_BASE, LOGGING_SIZE, MEM_RESERVE, PAGE_NOACCESS) ||
        !VirtualAllocEx(pi.hProcess, (void*)WSFIX_BASE, WSFIX_SIZE, MEM_RESERVE, PAGE_NOACCESS)) {
        TerminateProcess(pi.hProcess, 1); fail("reserve the palmont/pool/logging ranges in the child");
    }
    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code); return (int)code;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: nfsco_capture <game copy> <output dir> [seconds]\n"); return 2; }
    if (strcmp(argv[argc - 1], "--child") != 0) return run_child();
    --argc;
    if (!GetFullPathNameA(argv[2], MAX_PATH, g_out, NULL)) fail("output path");
    CreateDirectoryA(g_out, NULL);
    if (!SetCurrentDirectoryA(argv[1])) fail("game copy");
    SetDllDirectoryA(argv[1]);
    unsigned char* image = map_game("NFSC.exe");
    g_before = malloc(g_size); memcpy(g_before, image, g_size);
    VirtualFree((void*)LOGGING_BASE, 0, MEM_RELEASE);
    HMODULE logging = LoadLibraryA("logging.dll");
    if (logging != (HMODULE)LOGGING_BASE) fail("logging.dll not at its fixed base (use the rebase_fixed.py copy)");
    snapshot_regions();
    /* The ASI loader (dinput8.dll) loads scripts\*.asi with the game's imports,
     * before the bootstrapper attaches palmont: same order here. */
    /* Like NFSCO's bootstrapper, make the game image the process's main module
     * (PEB->ImageBaseAddress): ASI plugins scan GetModuleHandle(NULL). */
    ((PVOID*)__readfsdword(0x30))[2] = (PVOID)0x00400000;
    if (GetFileAttributesA("scripts\\NFSCarbon.WidescreenFix.asi") != INVALID_FILE_ATTRIBUTES) {
        VirtualFree((void*)WSFIX_BASE, 0, MEM_RELEASE);
        HMODULE wsfix = LoadLibraryA("scripts\\NFSCarbon.WidescreenFix.asi");
        if (wsfix != (HMODULE)WSFIX_BASE) fail("NFSCarbon.WidescreenFix.asi not at its fixed base (use the rebase_fixed.py copy)");
        printf("nfsco_capture: WidescreenFix at %p\n", (void*)wsfix);
    }
    VirtualFree((void*)PALMONT_BASE, 0, MEM_RELEASE);   /* free just for the loader */
    HMODULE palmont = LoadLibraryA("NFSCO\\bin\\palmont.dll");
    if (!palmont) fail("load palmont.dll");
    if (palmont != (HMODULE)PALMONT_BASE) fail("palmont.dll not at its fixed base (use the rebase_fixed.py copy)");
    hook_virtual_alloc(palmont);
    FARPROC attach = GetProcAddress(palmont, "OnAttach");
    if (!attach) fail("OnAttach export");
    if (argc > 3) g_wait_ms = (DWORD)atoi(argv[3]) * 1000;
    printf("nfsco_capture: palmont.dll at %p, calling OnAttach\n", (void*)palmont);
    fflush(stdout);
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    __try { ((void (*)(void))attach)(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { printf("nfsco_capture: OnAttach raised %08lX\n", GetExceptionCode()); }
    printf("nfsco_capture: OnAttach returned\n"); fflush(stdout);
    Sleep(g_wait_ms);   /* let threads it started apply late patches */
    dump_once();        /* never runs the game or palmont's threads further */
    return 0;
}
