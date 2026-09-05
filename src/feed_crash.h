// feed_crash.h -- what a crash line can say when the exception is a C++ throw.
//
// A C++ `throw` reaches an unhandled-exception filter as code 0xE06D7363 raised from
// inside KERNELBASE's RaiseException, so every one of them logs identically:
//
//   ### CRASH RECORDED ###  exception 0xE06D7363 at 00007FFB... in KERNELBASE.dll
//
// That names the messenger, never the thrower. It is the least useful crash line this
// project produces and, unlike an access violation, it carries no faulting address to
// reason about either -- which is how a report of "the new driver crashes the 32-bit
// path" arrived with a log that cannot distinguish ReShade from the neural consumer
// from the NVIDIA user-mode driver.
//
// Two things are recoverable at that moment and neither needs a debugger:
//
//  * WHAT was thrown. The MSVC throw ABI puts the thrown object and its ThrowInfo in
//    the exception record's parameters, and ThrowInfo carries the type descriptor for
//    the object and for every base it can be caught as. That gives a name -- and when
//    one of those bases is std::exception, the object's own what() gives the message.
//  * WHO threw it. The context record still describes the throwing frame, so an
//    ordinary table-driven unwind walks back through the callers; mapping each return
//    address to its module gives the chain that leads out of KERNELBASE and into
//    whoever actually raised. x64 only -- x86 has no unwind tables to walk.
//
// Header-only, nothing beyond kernel32, and everything that chases a pointer out of the
// exception record runs under __except: this code only ever runs on a thread that has
// already faulted once, and it must not be the reason a crash log goes missing.
//
// Requires <windows.h>. Included by both add-ons and by the host64 helper.

#pragma once

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <exception>

#define FEED_EXCEPTION_CXX 0xE06D7363u

// ---------------------------------------------------------------------------------------
// The MSVC throw ABI, as much of it as a type name needs.
//
// On x64 every pointer inside ThrowInfo is a 32-bit RVA from the module base, which the
// exception record supplies as a fourth parameter. On x86 the same fields hold absolute
// 32-bit pointers and there is no fourth parameter. Same structs, different resolver.
// ---------------------------------------------------------------------------------------
struct FeedThrowInfo          { unsigned attributes; int unwind; int fwd_compat; int catchable_array; };
struct FeedCatchableTypeArray { int count; int types[1]; };
struct FeedCatchableType      { unsigned properties; int type_desc; int mdisp, pdisp, vdisp; int size; int copy_fn; };
struct FeedTypeDescriptor     { void *vftable; void *spare; char name[1]; };

static const void *FeedThrowPtr(uintptr_t module_base, int field)
{
    if (field == 0) return nullptr;
#ifdef _WIN64
    return reinterpret_cast<const void *>(module_base + static_cast<uint32_t>(field));
#else
    (void)module_base;
    return reinterpret_cast<const void *>(static_cast<uintptr_t>(static_cast<uint32_t>(field)));
#endif
}

// The builtin type codes, for the throws that are not classes at all. `throw 42` and
// `throw "some message"` both happen in shipped middleware, and ".H" / ".PEBD" name
// neither of them to anyone reading a log.
static const char *FeedCrashBuiltinName(const char *code)
{
    struct Pair { const char *code, *name; };
    static const Pair kBuiltins[] = {
        { "C", "signed char" },   { "D", "char" },           { "E", "unsigned char" },
        { "F", "short" },         { "G", "unsigned short" }, { "H", "int" },
        { "I", "unsigned int" },  { "J", "long" },           { "K", "unsigned long" },
        { "M", "float" },         { "N", "double" },         { "O", "long double" },
        { "_J", "__int64" },      { "_K", "unsigned __int64" }, { "_N", "bool" },
        { "_W", "wchar_t" },
    };
    for (const Pair &b : kBuiltins)
        if (strcmp(code, b.code) == 0) return b.name;
    return nullptr;
}

// ".?AVbad_alloc@std@@" -> "std::bad_alloc". The decorated form is what the type
// descriptor stores, and it reads backwards: innermost name first, enclosing scopes after
// it, terminated by "@@". A leading pointer decoration is unwrapped too, because
// `throw new SomeError` is a real pattern and ".PEAVSomeError@@" hides it. Anything with a
// template or an unexpected shape in it is handed back undecorated rather than mangled
// further -- a raw ".?AV?$my_error@H@ns@@" is ugly but still identifies the thrower, which
// is the whole point.
static void FeedCrashTypeName(const char *decorated, char *out, size_t out_size)
{
    out[0] = '\0';
    if (decorated == nullptr || decorated[0] == '\0') return;

    const char *p = decorated;
    if (*p == '.') ++p;

    // "PEA"/"PEB" on x64, "PA"/"PB" on x86: pointer to, and B means pointer to const.
    const char *suffix = "";
    if (p[0] == 'P')
    {
        const char *q = p + 1;
        if (*q == 'E') ++q;                       // __ptr64
        if (*q == 'A')      { suffix = " *";       p = q + 1; }
        else if (*q == 'B') { suffix = " const *"; p = q + 1; }
    }

    // "?A" introduces a type descriptor for a plain class; a pointer to one has already
    // been unwrapped above and carries the class tag on its own ("PEAVSomeError@@").
    if (p[0] == '?' && p[1] == 'A') p += 2;
    const bool tagged = p[0] == 'V' || p[0] == 'U' || p[0] == 'T' || p[0] == 'W';
    if (!tagged)
    {
        if (const char *builtin = FeedCrashBuiltinName(p))
            _snprintf_s(out, out_size, _TRUNCATE, "%s%s", builtin, suffix);
        else
            strncpy_s(out, out_size, decorated, _TRUNCATE);
        return;
    }
    const bool is_enum = p[0] == 'W';
    p += 1;
    if (is_enum && *p == '4') ++p;   // ?AW4Name@@ -- the 4 is the enum's underlying type

    const char *part[8];
    size_t      len[8];
    int         n = 0;
    while (*p != '\0' && n < 8)
    {
        if (*p == '@') { ++p; if (*p == '@' || *p == '\0') break; }   // "@@" ends the name
        const char *start = p;
        while (*p != '\0' && *p != '@') ++p;
        if (p == start) break;
        // A template argument list or a back-reference is past what this is for.
        for (const char *q = start; q < p; ++q)
            if (*q == '?' || *q == '$') { strncpy_s(out, out_size, decorated, _TRUNCATE); return; }
        part[n] = start;
        len[n]  = static_cast<size_t>(p - start);
        ++n;
    }
    if (n == 0) { strncpy_s(out, out_size, decorated, _TRUNCATE); return; }

    size_t used = 0;
    for (int i = n - 1; i >= 0; --i)   // outermost scope first
    {
        const char  *sep     = used != 0 ? "::" : "";
        const size_t sep_len = strlen(sep);
        if (used + sep_len + len[i] + 1 > out_size) break;
        memcpy(out + used, sep, sep_len);
        used += sep_len;
        memcpy(out + used, part[i], len[i]);
        used += len[i];
    }
    out[used] = '\0';
    if (used == 0) { strncpy_s(out, out_size, decorated, _TRUNCATE); return; }
    if (suffix[0] != '\0' && used + strlen(suffix) + 1 <= out_size) strcpy_s(out + used, out_size - used, suffix);
}

// The thrown object's own message, when it can be caught as a std::exception.
//
// ThrowInfo lists every type the object is catchable as, each with the displacement from
// the thrown pointer to that base. Finding std::exception among them is both the proof
// that what() exists and the adjustment needed to call it. A virtual base (pdisp >= 0) is
// skipped: no standard exception uses one, and following a vbtable is not worth the
// pointer chase on a thread that has already faulted.
static bool FeedCrashWhat(const void *obj, const FeedCatchableTypeArray *cta, uintptr_t base,
                          char *out, size_t out_size)
{
    out[0] = '\0';
    if (obj == nullptr || cta == nullptr) return false;
    const int count = cta->count;
    if (count <= 0 || count > 64) return false;

    for (int i = 0; i < count; ++i)
    {
        const FeedCatchableType *ct =
            static_cast<const FeedCatchableType *>(FeedThrowPtr(base, cta->types[i]));
        if (ct == nullptr) continue;
        const FeedTypeDescriptor *td =
            static_cast<const FeedTypeDescriptor *>(FeedThrowPtr(base, ct->type_desc));
        if (td == nullptr) continue;
        if (strcmp(td->name, ".?AVexception@std@@") != 0) continue;
        if (ct->pdisp >= 0) return false;

        const std::exception *e = reinterpret_cast<const std::exception *>(
            static_cast<const char *>(obj) + ct->mdisp);
        const char *msg = e->what();
        if (msg == nullptr || msg[0] == '\0') return false;
        strncpy_s(out, out_size, msg, _TRUNCATE);
        // A newline in the message would break the one-line-per-event log format.
        for (char *s = out; *s != '\0'; ++s)
            if (*s == '\r' || *s == '\n' || *s == '\t') *s = ' ';
        return true;
    }
    return false;
}

// Fills `out` with " (C++ exception: std::bad_alloc -- \"...\")" -- leading separator
// included, so callers append it straight onto the crash line -- and returns true. Returns
// false for any exception code that is not a C++ throw.
static bool FeedCrashDescribeCxx(const EXCEPTION_RECORD *r, char *out, size_t out_size)
{
    out[0] = '\0';
    if (r == nullptr || r->ExceptionCode != FEED_EXCEPTION_CXX) return false;

    char type[256] = "", what[320] = "";
    bool have_what = false;

    if (r->NumberParameters < 3 || (r->ExceptionInformation[0] & ~static_cast<ULONG_PTR>(0xFF)) != 0x19930500u)
    {
        // A C++ throw raised by a runtime whose record this cannot read: MinGW, a foreign
        // EH implementation, or a magic number newer than this build. Say so, do not guess.
        _snprintf_s(out, out_size, _TRUNCATE,
                    " (C++ exception; throw record not in a layout this build can read)");
        return true;
    }
    if (r->ExceptionInformation[2] == 0)
    {
        // A bare `throw;` inside a catch block re-raises with no ThrowInfo.
        _snprintf_s(out, out_size, _TRUNCATE, " (C++ exception, rethrown -- no type information)");
        return true;
    }

    __try
    {
        const void          *obj = reinterpret_cast<const void *>(r->ExceptionInformation[1]);
        const FeedThrowInfo *ti  = reinterpret_cast<const FeedThrowInfo *>(r->ExceptionInformation[2]);
#ifdef _WIN64
        const uintptr_t mbase = r->NumberParameters >= 4
                              ? static_cast<uintptr_t>(r->ExceptionInformation[3]) : 0;
#else
        const uintptr_t mbase = 0;
#endif
        const FeedCatchableTypeArray *cta =
            static_cast<const FeedCatchableTypeArray *>(FeedThrowPtr(mbase, ti->catchable_array));
        if (cta != nullptr && cta->count > 0)
        {
            // The first catchable type is the object's own most-derived type.
            const FeedCatchableType *ct =
                static_cast<const FeedCatchableType *>(FeedThrowPtr(mbase, cta->types[0]));
            const FeedTypeDescriptor *td = ct != nullptr
                ? static_cast<const FeedTypeDescriptor *>(FeedThrowPtr(mbase, ct->type_desc)) : nullptr;
            if (td != nullptr) FeedCrashTypeName(td->name, type, sizeof(type));
            have_what = FeedCrashWhat(obj, cta, mbase, what, sizeof(what));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        type[0]   = '\0';
        have_what = false;
    }

    if (type[0] == '\0') strncpy_s(type, "type unreadable", _TRUNCATE);
    if (have_what) _snprintf_s(out, out_size, _TRUNCATE, " (C++ exception: %s -- \"%s\")", type, what);
    else           _snprintf_s(out, out_size, _TRUNCATE, " (C++ exception: %s)", type);
    return true;
}

// An access violation carries two extra words, and they are the difference between "it read
// a null pointer" and "we wrote off the end of something". Free to log, and until this was
// recorded the only way to recover it was to open the minidump by hand -- which is exactly
// what issue #44 (Bayonetta) needed to establish that the fault was a read of address 0 in
// the game's own code, before this add-on had fed a single frame.
static bool FeedCrashAccessDetail(const EXCEPTION_RECORD *r, char *out, size_t out_size)
{
    out[0] = '\0';
    if (r == nullptr || r->NumberParameters < 2) return false;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION && r->ExceptionCode != EXCEPTION_IN_PAGE_ERROR)
        return false;
    const char *verb = r->ExceptionInformation[0] == 0 ? "reading"
                     : r->ExceptionInformation[0] == 1 ? "writing"
                     : r->ExceptionInformation[0] == 8 ? "executing" : "accessing";
    _snprintf_s(out, out_size, _TRUNCATE, " (%s address %p)", verb,
                reinterpret_cast<void *>(r->ExceptionInformation[1]));
    return true;
}

// Whichever of the two applies, or "" for an exception that is neither.
static void FeedCrashDescribe(const EXCEPTION_RECORD *r, char *out, size_t out_size)
{
    if (FeedCrashDescribeCxx(r, out, out_size)) return;
    if (FeedCrashAccessDetail(r, out, out_size)) return;
    out[0] = '\0';
}

// The module an address belongs to, by file name -- "nvngx_dlssnr.dll", not a path and not
// a bare pointer. Falls back to a printed address for memory that belongs to no module.
static void FeedCrashModuleOf(const void *addr, char *out, size_t out_size)
{
    HMODULE m = nullptr;
    if (addr != nullptr &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(addr), &m) && m != nullptr)
    {
        wchar_t path[MAX_PATH] = L"";
        GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t *leaf = wcsrchr(path, L'\\');
        _snprintf_s(out, out_size, _TRUNCATE, "%ls", leaf != nullptr ? leaf + 1 : path);
    }
    else
        _snprintf_s(out, out_size, _TRUNCATE, "no module (%p)", addr);
}

// ---------------------------------------------------------------------------------------
// The modules on the faulting stack, innermost frame first.
//
// Table-driven unwinding needs no symbols and no dbghelp: RtlLookupFunctionEntry finds the
// function containing an address, RtlVirtualUnwind steps the context back one frame. Only
// the distinct modules are reported -- "KERNELBASE.dll <- nvwgf2umx.dll <- dxgi.dll"
// answers the question a crash line is actually asked, and the frame-by-frame detail is in
// the minidump for anyone who needs it.
//
// x64 only. x86 unwinds by convention rather than by table, so there is nothing to walk
// without symbols; that side gets the thrown type and nothing more.
// ---------------------------------------------------------------------------------------
static void FeedCrashStackModules(const CONTEXT *ctx, char *out, size_t out_size)
{
    out[0] = '\0';
#ifdef _WIN64
    if (ctx == nullptr || out_size < 32) return;

    CONTEXT c     = *ctx;
    HMODULE seen[12];
    int     nseen = 0;
    size_t  used  = 0;

    __try
    {
        for (int frame = 0; frame < 96 && c.Rip != 0; ++frame)
        {
            HMODULE m = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(c.Rip), &m) && m != nullptr)
            {
                bool known = false;
                for (int i = 0; i < nseen; ++i) if (seen[i] == m) { known = true; break; }
                if (!known)
                {
                    if (nseen >= static_cast<int>(sizeof(seen) / sizeof(seen[0]))) break;
                    seen[nseen++] = m;

                    wchar_t path[MAX_PATH] = L"";
                    GetModuleFileNameW(m, path, MAX_PATH);
                    const wchar_t *leaf = wcsrchr(path, L'\\');
                    leaf = leaf != nullptr ? leaf + 1 : path;
                    const int n = _snprintf_s(out + used, out_size - used, _TRUNCATE,
                                              "%s%ls", used != 0 ? " <- " : "", leaf);
                    if (n < 0) break;   // truncated: the useful end of the chain is already in
                    used += static_cast<size_t>(n);
                }
            }

            DWORD64           image = 0;
            PRUNTIME_FUNCTION fn    = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
            if (fn == nullptr)
            {
                // A leaf function has no unwind data: its return address is at RSP.
                if (c.Rsp == 0) break;
                c.Rip  = *reinterpret_cast<const DWORD64 *>(c.Rsp);
                c.Rsp += 8;
            }
            else
            {
                void   *handler_data = nullptr;
                DWORD64 establisher  = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, fn, &c,
                                 &handler_data, &establisher, nullptr);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Whatever the walk collected before it went off the rails is still worth logging.
    }
#else
    (void)ctx;
#endif
}
