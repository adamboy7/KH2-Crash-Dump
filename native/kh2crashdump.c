/*
 * kh2crashdump - crash dumps for LuaBackend games (Lua 5.4.8 module, Lua core embedded).
 *
 * When the game crashes, writes <prefix>-<yyyymmdd-hhmmss>.dmp (a Windows minidump that
 * WinDbg, Visual Studio or Python's minidump package can open) and appends a text record to
 * <prefix>.log: the exception, the address as an offset into the game exe, registers, and
 * the game-exe addresses found on the stack.
 *
 *   local cd = require("kh2crashdump")
 *   cd.arm{ prefix = "KH2Crash", full = false, keep = 5 }   -> true | nil, err
 *   cd.keep()      -- call now and then: puts the filter back if something replaced it
 *   cd.version()   -> integer
 *
 * Only real crashes count: the hook is the process's unhandled-exception filter, which
 * Windows calls after every exception handler has had its chance, so exceptions that the
 * game or another mod catches (LuaBackend's reads of bad addresses, for example) never make
 * a dump. The filter that was installed before ours is still called afterwards.
 *
 * The dump is written by a helper thread made when the module is armed. A crashing thread
 * can be out of stack (stack overflows, or a deep crash), and MiniDumpWriteDump needs a lot
 * of it: written in place, the file came out empty (0 bytes), as Panacea's CrashDump.dmp
 * does. MiniDumpWriteDump comes from System32\dbgcore.dll by full path, because the game
 * folder's DBGHELP.dll is Panacea's proxy.
 *
 * Dump contents: threads and their stacks, the data sections of every module (KH2's work
 * memory and heaps live in the exe's own data), and memory that stacks and data point at.
 * full = true adds every private read/write page (gigabytes; can fail part-way, in which
 * case the small dump is written instead).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lua.h"
#include "lauxlib.h"

#define VERSION 1

typedef unsigned long long u64;
typedef BOOL (WINAPI *MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, int, void *, void *, void *);

#pragma pack(push, 4)   /* dbghelp.h packs the minidump structs to 4 */
typedef struct { DWORD ThreadId; PEXCEPTION_POINTERS ExceptionPointers; BOOL ClientPointers; } MDEI;
#pragma pack(pop)

static char g_prefix[MAX_PATH];
static int g_full, g_keep = 5;
static MiniDumpWriteDump_t g_mdwd;
static HANDLE g_go, g_done;
static PEXCEPTION_POINTERS g_ep;
static DWORD g_tid;
static u64 g_stack_top;
static volatile LONG g_fired;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prev;

static int push_msg(lua_State *L, const char *msg) { lua_pushnil(L); lua_pushstring(L, msg); return 2; }

static void out(HANDLE h, const char *s) { DWORD w; WriteFile(h, s, (DWORD)strlen(s), &w, NULL); }

static void note(const char *s) {
    char path[MAX_PATH + 8];
    snprintf(path, sizeof path, "%s.log", g_prefix);
    HANDLE h = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    out(h, s);
    CloseHandle(h);
}

static const char *code_name(DWORD c) {
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case 0xC0000374: return "heap corruption";
    case 0xC0000409: return "stack buffer overrun";
    case 0xE06D7363: return "C++ exception";
    default: return "exception";
    }
}

/* the text record: what crashed, where, registers, game-exe addresses on the stack */
static void write_record(const char *dump_result) {
    EXCEPTION_RECORD *er = g_ep->ExceptionRecord;
    CONTEXT *c = g_ep->ContextRecord;
    u64 base = (u64)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    u64 end = base + nt->OptionalHeader.SizeOfImage;
    u64 ip = (u64)er->ExceptionAddress;
    char b[1024], mod[MAX_PATH] = "?";
    HMODULE hm = NULL;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)ip, &hm) && hm) {
        char full[MAX_PATH];
        if (GetModuleFileNameA(hm, full, sizeof full)) {
            const char *s = strrchr(full, '\\');
            strncpy(mod, s ? s + 1 : full, sizeof mod - 1);
        }
    }
    SYSTEMTIME t; GetLocalTime(&t);
    snprintf(b, sizeof b, "\r\n%04d-%02d-%02d %02d:%02d:%02d crash: %s (0x%08lX) at %llX = %s+%llX, thread %lu\r\n",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, code_name(er->ExceptionCode),
             (unsigned long)er->ExceptionCode, ip, mod, hm ? ip - (u64)hm : 0ULL, (unsigned long)g_tid);
    note(b);
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
        const char *k = er->ExceptionInformation[0] == 0 ? "read" : er->ExceptionInformation[0] == 1 ? "write" : "execute";
        snprintf(b, sizeof b, "  %s of address %llX\r\n", k, (u64)er->ExceptionInformation[1]);
        note(b);
    }
    snprintf(b, sizeof b, "  rax %llX rbx %llX rcx %llX rdx %llX\r\n  rsi %llX rdi %llX rbp %llX rsp %llX\r\n"
             "  r8 %llX r9 %llX r10 %llX r11 %llX\r\n  r12 %llX r13 %llX r14 %llX r15 %llX\r\n",
             c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp,
             c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15);
    note(b);
    /* stack: every qword that points into the game exe (return addresses, mostly) */
    int n = 0, o = snprintf(b, sizeof b, "  game exe offsets on the stack:");
    for (u64 *sp = (u64 *)c->Rsp; (u64)sp < g_stack_top && sp < (u64 *)c->Rsp + 4096 && n < 40; sp++) {
        MEMORY_BASIC_INFORMATION mi;
        if (((u64)sp & 0xFFF) == 0 && (!VirtualQuery(sp, &mi, sizeof mi) || mi.State != MEM_COMMIT)) break;
        u64 v = *sp;
        if (v > base + 0x1000 && v < end) {
            if (o > (int)sizeof b - 24) { note(b); note("\r\n"); o = snprintf(b, sizeof b, "   "); }
            o += snprintf(b + o, sizeof b - o, " %llX", v - base);
            n++;
        }
    }
    note(b);
    note("\r\n");
    note(dump_result);
}

/* oldest dumps beyond g_keep are deleted (names sort by time) */
static int cmp_names(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }
static void prune_dumps(int keep_new) {
    if (g_keep <= 0) return;
    char pat[MAX_PATH + 16], dir[MAX_PATH] = "";
    snprintf(pat, sizeof pat, "%s-*.dmp", g_prefix);
    const char *slash = strrchr(g_prefix, '\\');
    if (slash) { size_t n = (size_t)(slash - g_prefix) + 1; memcpy(dir, g_prefix, n); dir[n] = 0; }
    static char names[256][MAX_PATH];
    int count = 0;
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do { if (count < 256) strncpy(names[count++], fd.cFileName, MAX_PATH - 1); } while (FindNextFileA(f, &fd));
    FindClose(f);
    qsort(names, count, MAX_PATH, cmp_names);
    for (int i = 0; i < count - (g_keep - keep_new); i++) {
        char p[MAX_PATH * 2];
        snprintf(p, sizeof p, "%s%s", dir, names[i]);
        DeleteFileA(p);
    }
}

static DWORD WINAPI dump_thread(LPVOID unused) {
    (void)unused;
    WaitForSingleObject(g_go, INFINITE);
    char path[MAX_PATH + 32], res[MAX_PATH + 128];
    SYSTEMTIME t; GetLocalTime(&t);
    snprintf(path, sizeof path, "%s-%04d%02d%02d-%02d%02d%02d.dmp", g_prefix,
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    prune_dumps(1);
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        snprintf(res, sizeof res, "  dump FAILED: cannot create %s (error %lu)\r\n", path, GetLastError());
    } else {
        MDEI ei = { g_tid, g_ep, FALSE };
        int small = 0x1 /* DataSegs */ | 0x4 /* HandleData */ | 0x20 /* UnloadedModules */ |
                    0x40 /* IndirectlyReferencedMemory */ | 0x100 /* ProcessThreadData */ |
                    0x800 /* FullMemoryInfo */ | 0x1000 /* ThreadInfo */;
        int type = g_full ? small | 0x200 /* PrivateReadWriteMemory */ : small;
        BOOL ok = g_mdwd(GetCurrentProcess(), GetCurrentProcessId(), h, type, &ei, NULL, NULL);
        DWORD err = ok ? 0 : GetLastError();
        const char *how = g_full ? "full" : "normal";
        if (!ok) {   /* try again with the most basic dump */
            SetFilePointer(h, 0, NULL, FILE_BEGIN);
            SetEndOfFile(h);
            ok = g_mdwd(GetCurrentProcess(), GetCurrentProcessId(), h,
                        0x1 /* DataSegs */ | 0x100 | 0x1000, &ei, NULL, NULL);
            how = "basic (the larger dump failed)";
        }
        LARGE_INTEGER sz = { 0 };
        GetFileSizeEx(h, &sz);
        CloseHandle(h);
        if (ok) snprintf(res, sizeof res, "  dump written (%s, %.1f MB): %s\r\n", how, sz.QuadPart / 1048576.0, path);
        else snprintf(res, sizeof res, "  dump FAILED (error 0x%08lX): %s\r\n", (unsigned long)err, path);
    }
    write_record(res);
    SetEvent(g_done);
    return 0;
}

static LONG WINAPI crash_filter(PEXCEPTION_POINTERS ep) {
    /* runs on the crashing thread, perhaps with almost no stack left: hand over and wait */
    if (InterlockedIncrement(&g_fired) == 1) {
        g_ep = ep;
        g_tid = GetCurrentThreadId();
        g_stack_top = (u64)((NT_TIB *)NtCurrentTeb())->StackBase;
        SetEvent(g_go);
        WaitForSingleObject(g_done, 180000);
    }
    if (g_prev && g_prev != crash_filter) return g_prev(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void set_filter(void) {
    LPTOP_LEVEL_EXCEPTION_FILTER p = SetUnhandledExceptionFilter(crash_filter);
    if (p != crash_filter) g_prev = p;
}

static int l_arm(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "prefix");
    const char *prefix = luaL_optstring(L, -1, "KH2Crash");
    if (!GetFullPathNameA(prefix, sizeof g_prefix, g_prefix, NULL)) strncpy(g_prefix, prefix, sizeof g_prefix - 1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "full"); g_full = lua_toboolean(L, -1); lua_pop(L, 1);
    lua_getfield(L, 1, "keep"); g_keep = (int)luaL_optinteger(L, -1, 5); lua_pop(L, 1);
    if (!g_mdwd) {
        wchar_t sys[MAX_PATH];
        UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        if (n && n < MAX_PATH - 16) {
            wcscat(sys, L"\\dbgcore.dll");
            HMODULE m = LoadLibraryW(sys);
            if (m) g_mdwd = (MiniDumpWriteDump_t)GetProcAddress(m, "MiniDumpWriteDump");
        }
        if (!g_mdwd) return push_msg(L, "MiniDumpWriteDump not found in System32\\dbgcore.dll");
    }
    if (!g_go) {
        g_done = CreateEventA(NULL, TRUE, FALSE, NULL);
        HANDLE go = CreateEventA(NULL, TRUE, FALSE, NULL);
        HANDLE th = (go && g_done) ? CreateThread(NULL, 8 << 20, dump_thread, NULL, 0, NULL) : NULL;
        if (!th) return push_msg(L, "could not start the dump thread");
        CloseHandle(th);
        g_go = go;
    }
    set_filter();
    lua_pushstring(L, g_prefix);
    return 1;
}

static int l_keep(lua_State *L) { (void)L; if (g_go) set_filter(); return 0; }
static int l_version(lua_State *L) { lua_pushinteger(L, VERSION); return 1; }

static const luaL_Reg funcs[] = {
    {"arm", l_arm},
    {"keep", l_keep},
    {"version", l_version},
    {NULL, NULL}
};

__declspec(dllexport) int luaopen_kh2crashdump(lua_State *L) {
    /* pinned: the filter must outlive a script reload (F1), which unloads C modules */
    HMODULE self;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       (LPCWSTR)(void *)&luaopen_kh2crashdump, &self);
    luaL_newlib(L, funcs);
    return 1;
}
