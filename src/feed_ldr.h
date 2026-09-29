// feed_ldr.h - module load/unload notifications from the Windows loader.
//
// LdrRegisterDllNotification is an ntdll export since Vista, documented on MSDN but with no
// import library, so it is resolved at run time. The callback runs on the thread that loads or
// unloads the module, with the loader lock held. For an unload it runs after that module's
// DLL_PROCESS_DETACH and before its image is unmapped: the code bytes are still there, which
// is what lets a hook be put back before the address range goes free (#135).
//
// Rules inside the callback: no LoadLibrary/FreeLibrary, no waiting on another thread, nothing
// that could need the loader lock from a different thread. Log(), MinHook's enable/disable and
// CreateThread (the new thread simply starts once the lock is released) are all fine.

#pragma once
#include <windows.h>

struct FeedUnicodeString { USHORT Length; USHORT MaximumLength; PWSTR Buffer; };

// LDR_DLL_LOADED_NOTIFICATION_DATA and LDR_DLL_UNLOADED_NOTIFICATION_DATA share this layout.
struct FeedLdrDllNotificationData
{
    ULONG                    Flags;
    const FeedUnicodeString *FullDllName;
    const FeedUnicodeString *BaseDllName;
    PVOID                    DllBase;
    ULONG                    SizeOfImage;
};

enum : ULONG { FEED_LDR_LOADED = 1, FEED_LDR_UNLOADED = 2 };

typedef VOID (CALLBACK *FeedLdrNotifyFn)(ULONG reason, const FeedLdrDllNotificationData *data, PVOID context);

typedef BOOLEAN (NTAPI *PFN_FeedRtlDllShutdownInProgress)();
static PFN_FeedRtlDllShutdownInProgress g_feed_ldr_shutdown_fn;   // resolved by FeedLdrWatch

// Returns the cookie to hand back to FeedLdrUnwatch, or nullptr when the loader refused.
static PVOID FeedLdrWatch(FeedLdrNotifyFn fn)
{
    typedef LONG (NTAPI *PFN_Register)(ULONG, FeedLdrNotifyFn, PVOID, PVOID *);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto reg = ntdll != nullptr
        ? reinterpret_cast<PFN_Register>(GetProcAddress(ntdll, "LdrRegisterDllNotification")) : nullptr;
    if (ntdll != nullptr)
        g_feed_ldr_shutdown_fn = reinterpret_cast<PFN_FeedRtlDllShutdownInProgress>(
            GetProcAddress(ntdll, "RtlDllShutdownInProgress"));
    PVOID cookie = nullptr;
    if (reg == nullptr || reg(0, fn, nullptr, &cookie) < 0) return nullptr;
    return cookie;
}

// From DLL_PROCESS_DETACH, before anything else: the callback is code in this module.
static void FeedLdrUnwatch(PVOID &cookie)
{
    if (cookie == nullptr) return;
    typedef LONG (NTAPI *PFN_Unregister)(PVOID);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto unreg = ntdll != nullptr
        ? reinterpret_cast<PFN_Unregister>(GetProcAddress(ntdll, "LdrUnregisterDllNotification")) : nullptr;
    if (unreg != nullptr) unreg(cookie);
    cookie = nullptr;
}

// True while the process is exiting: no thread started from here would ever run.
static bool FeedLdrShuttingDown()
{
    return g_feed_ldr_shutdown_fn != nullptr && g_feed_ldr_shutdown_fn() != FALSE;
}
