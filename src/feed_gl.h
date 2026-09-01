// feed_gl.h - raw-OpenGL interop for the OpenGL transport (PLAN-OPENGL).
//
// The GL side of the transport is entirely raw, unlike the Vulkan one, for two
// reasons established in the plan:
//
//  * ReShade's api::fence on OpenGL is "an opaque value", NOT a GL semaphore name
//    (reshade_api_pipeline.hpp), so the Vulkan trick of importing raw and handing
//    the object back to ReShade for command_queue::signal/wait has no GL analogue.
//  * ReShade's create_resource/create_fence import shared handles as OPAQUE_WIN32,
//    which a D3D12-created handle is not -- the same finding that produced feed_vk.h.
//
// Raw GL is safe here where a raw vkQueueSubmit was not: OpenGL has no queue object.
// Every command enters the current context's single in-order stream on the calling
// thread, and reshade_render_technique fires while ReShade is itself issuing GL
// commands on that thread and context. Our calls simply interleave in program order.
//
// Direction of creation is forced: GL memory objects are import-only (there is no
// memory-object export in GL_EXT_external_objects_win32), so D3D12 creates and GL
// imports -- the same one-way lesson as D3D11 and Vulkan.
//
// Everything is resolved at runtime from the already-loaded opengl32.dll and
// wglGetProcAddress on whatever context is current; there is no link-time GL
// dependency (and there could not be a sane one: when ReShade is installed for
// OpenGL it IS the local opengl32.dll). Tokens and typedefs below are copied from
// the Khronos registry glext.h, so the header stays self-contained like feed_ipc.h.
//
// Compiles for x64 (the in-process 64-bit path) and x86 (the 32-bit stub, which
// imports handles served by the 64-bit host and therefore has no D3D12 device --
// hence every size the import needs is passed in explicitly).

#pragma once
#include <gl/GL.h>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>

// ---------------------------------------------------------------------------
// Types and tokens the Windows SDK's <gl/GL.h> (an OpenGL 1.1 header) lacks
// ---------------------------------------------------------------------------

typedef uint64_t GLuint64_feed;
typedef char     GLchar_feed;

#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS                 0x821D
#endif

// GL_EXT_memory_object / GL_EXT_semaphore (GL_EXT_external_objects)
#define GL_TEXTURE_TILING_EXT             0x9580
#define GL_DEDICATED_MEMORY_OBJECT_EXT    0x9581
#define GL_PROTECTED_MEMORY_OBJECT_EXT    0x959B
#define GL_OPTIMAL_TILING_EXT             0x9584
#define GL_LINEAR_TILING_EXT              0x9585
#define GL_LAYOUT_GENERAL_EXT             0x958D
#define GL_LAYOUT_COLOR_ATTACHMENT_EXT    0x958E
#define GL_LAYOUT_SHADER_READ_ONLY_EXT    0x9591
#define GL_LAYOUT_TRANSFER_SRC_EXT        0x9592
#define GL_LAYOUT_TRANSFER_DST_EXT        0x9593

// GL_EXT_memory_object_win32 / GL_EXT_semaphore_win32
#define GL_HANDLE_TYPE_OPAQUE_WIN32_EXT      0x9587
#define GL_HANDLE_TYPE_OPAQUE_WIN32_KMT_EXT  0x9588
#define GL_HANDLE_TYPE_D3D12_TILEPOOL_EXT    0x9589
#define GL_HANDLE_TYPE_D3D12_RESOURCE_EXT    0x958A
#define GL_HANDLE_TYPE_D3D11_IMAGE_EXT       0x958B
#define GL_HANDLE_TYPE_D3D11_IMAGE_KMT_EXT   0x958C
#define GL_HANDLE_TYPE_D3D12_FENCE_EXT       0x9594
#define GL_D3D12_FENCE_VALUE_EXT             0x9595

// Framebuffer objects / blit (GL 3.0)
#define GL_FRAMEBUFFER                    0x8D40
#define GL_READ_FRAMEBUFFER               0x8CA8
#define GL_DRAW_FRAMEBUFFER               0x8CA9
#define GL_DRAW_FRAMEBUFFER_BINDING       0x8CA6
#define GL_READ_FRAMEBUFFER_BINDING       0x8CAA
#define GL_RENDERBUFFER                   0x8D41
#define GL_COLOR_ATTACHMENT0              0x8CE0
#define GL_FRAMEBUFFER_COMPLETE           0x8CD5
#define GL_FRAMEBUFFER_SRGB               0x8DB9
#define GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING 0x8210
#ifndef GL_SRGB
#define GL_SRGB                           0x8C40
#endif

// Sync objects (GL 3.2), for the one place a bounded wait is needed
#define GL_SYNC_GPU_COMMANDS_COMPLETE     0x9117
#define GL_SYNC_FLUSH_COMMANDS_BIT        0x00000001
#define GL_ALREADY_SIGNALED               0x911A
#define GL_TIMEOUT_EXPIRED                0x911B
#define GL_CONDITION_SATISFIED            0x911C
#define GL_WAIT_FAILED                    0x911D

// Sized internal formats newer than 1.1
#define GL_RGBA16F                        0x881A
#define GL_R11F_G11F_B10F                 0x8C3A
#define GL_R32F                           0x822E
#define GL_RG16F                          0x822F
#define GL_R8                             0x8229

// ---------------------------------------------------------------------------
// Entry-point table
// ---------------------------------------------------------------------------

typedef void (APIENTRY *PFN_glGetIntegerv_)(GLenum, GLint *);
typedef const GLubyte * (APIENTRY *PFN_glGetString_)(GLenum);
typedef const GLubyte * (APIENTRY *PFN_glGetStringi_)(GLenum, GLuint);
typedef GLenum (APIENTRY *PFN_glGetError_)(void);
typedef void (APIENTRY *PFN_glGenTextures_)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glDeleteTextures_)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glBindTexture_)(GLenum, GLuint);
typedef void (APIENTRY *PFN_glFlush_)(void);
typedef void (APIENTRY *PFN_glFinish_)(void);
typedef void (APIENTRY *PFN_glEnable_)(GLenum);
typedef void (APIENTRY *PFN_glDisable_)(GLenum);
typedef GLboolean (APIENTRY *PFN_glIsEnabled_)(GLenum);
typedef void (APIENTRY *PFN_glReadBuffer_)(GLenum);
typedef void (APIENTRY *PFN_glDrawBuffer_)(GLenum);

typedef void (APIENTRY *PFN_glGenFramebuffers_)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glDeleteFramebuffers_)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glBindFramebuffer_)(GLenum, GLuint);
typedef void (APIENTRY *PFN_glFramebufferTexture2D_)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (APIENTRY *PFN_glFramebufferRenderbuffer_)(GLenum, GLenum, GLenum, GLuint);
typedef void (APIENTRY *PFN_glBlitFramebuffer_)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
typedef GLenum (APIENTRY *PFN_glCheckFramebufferStatus_)(GLenum);
typedef void (APIENTRY *PFN_glGetFramebufferAttachmentParameteriv_)(GLenum, GLenum, GLenum, GLint *);
typedef void (APIENTRY *PFN_glCopyImageSubData_)(GLuint, GLenum, GLint, GLint, GLint, GLint,
                                                 GLuint, GLenum, GLint, GLint, GLint, GLint,
                                                 GLsizei, GLsizei, GLsizei);

typedef void * GLsync_feed;
typedef GLsync_feed (APIENTRY *PFN_glFenceSync_)(GLenum, GLbitfield);
typedef GLenum (APIENTRY *PFN_glClientWaitSync_)(GLsync_feed, GLbitfield, GLuint64_feed);
typedef void (APIENTRY *PFN_glDeleteSync_)(GLsync_feed);

typedef void (APIENTRY *PFN_glCreateMemoryObjectsEXT_)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glDeleteMemoryObjectsEXT_)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glMemoryObjectParameterivEXT_)(GLuint, GLenum, const GLint *);
typedef void (APIENTRY *PFN_glTexStorageMem2DEXT_)(GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64_feed);
typedef void (APIENTRY *PFN_glTextureStorageMem2DEXT_)(GLuint, GLsizei, GLenum, GLsizei, GLsizei, GLuint, GLuint64_feed);
typedef void (APIENTRY *PFN_glImportMemoryWin32HandleEXT_)(GLuint, GLuint64_feed, GLenum, void *);

typedef void (APIENTRY *PFN_glGenSemaphoresEXT_)(GLsizei, GLuint *);
typedef void (APIENTRY *PFN_glDeleteSemaphoresEXT_)(GLsizei, const GLuint *);
typedef void (APIENTRY *PFN_glSemaphoreParameterui64vEXT_)(GLuint, GLenum, const GLuint64_feed *);
typedef void (APIENTRY *PFN_glWaitSemaphoreEXT_)(GLuint, GLuint, const GLuint *, GLuint, const GLuint *, const GLenum *);
typedef void (APIENTRY *PFN_glSignalSemaphoreEXT_)(GLuint, GLuint, const GLuint *, GLuint, const GLuint *, const GLenum *);
typedef void (APIENTRY *PFN_glImportSemaphoreWin32HandleEXT_)(GLuint, GLenum, void *);

typedef PROC (WINAPI *PFN_wglGetProcAddress_)(LPCSTR);
typedef HGLRC (WINAPI *PFN_wglGetCurrentContext_)(void);

struct FeedGl
{
    HMODULE lib;   // opengl32.dll, NOT owned (never LoadLibrary'd: it is already loaded,
                   // and with ReShade installed for OpenGL it IS ReShade)

    PFN_wglGetProcAddress_   wglGetProcAddress;
    PFN_wglGetCurrentContext_ wglGetCurrentContext;

    PFN_glGetIntegerv_      GetIntegerv;
    PFN_glGetString_        GetString;
    PFN_glGetStringi_       GetStringi;
    PFN_glGetError_         GetError;
    PFN_glGenTextures_      GenTextures;
    PFN_glDeleteTextures_   DeleteTextures;
    PFN_glBindTexture_      BindTexture;
    PFN_glFlush_            Flush;
    PFN_glFinish_           Finish;
    PFN_glEnable_           Enable;
    PFN_glDisable_          Disable;
    PFN_glIsEnabled_        IsEnabled;
    PFN_glReadBuffer_       ReadBuffer;
    PFN_glDrawBuffer_       DrawBuffer;

    PFN_glGenFramebuffers_        GenFramebuffers;
    PFN_glDeleteFramebuffers_     DeleteFramebuffers;
    PFN_glBindFramebuffer_        BindFramebuffer;
    PFN_glFramebufferTexture2D_   FramebufferTexture2D;
    PFN_glFramebufferRenderbuffer_ FramebufferRenderbuffer;
    PFN_glBlitFramebuffer_        BlitFramebuffer;
    PFN_glCheckFramebufferStatus_ CheckFramebufferStatus;
    PFN_glGetFramebufferAttachmentParameteriv_ GetFramebufferAttachmentParameteriv;
    PFN_glCopyImageSubData_       CopyImageSubData;

    PFN_glFenceSync_      FenceSync;
    PFN_glClientWaitSync_ ClientWaitSync;
    PFN_glDeleteSync_     DeleteSync;

    PFN_glCreateMemoryObjectsEXT_     CreateMemoryObjectsEXT;
    PFN_glDeleteMemoryObjectsEXT_     DeleteMemoryObjectsEXT;
    PFN_glMemoryObjectParameterivEXT_ MemoryObjectParameterivEXT;
    PFN_glTexStorageMem2DEXT_         TexStorageMem2DEXT;
    PFN_glTextureStorageMem2DEXT_     TextureStorageMem2DEXT;   // optional (DSA); may be null
    PFN_glImportMemoryWin32HandleEXT_ ImportMemoryWin32HandleEXT;

    PFN_glGenSemaphoresEXT_             GenSemaphoresEXT;
    PFN_glDeleteSemaphoresEXT_          DeleteSemaphoresEXT;
    PFN_glSemaphoreParameterui64vEXT_   SemaphoreParameterui64vEXT;
    PFN_glWaitSemaphoreEXT_             WaitSemaphoreEXT;
    PFN_glSignalSemaphoreEXT_           SignalSemaphoreEXT;
    PFN_glImportSemaphoreWin32HandleEXT_ ImportSemaphoreWin32HandleEXT;

    // Filled by FeedGlLoad for the caller to log verbatim.
    char missing[256];      // the first extension/entry point that was absent
    char renderer[128];     // GL_RENDERER, so a wrong-GPU hybrid laptop is obvious
    char version[64];       // GL_VERSION
    char diag[512];         // how the extension query behaved (see FeedGlSurveyExtensions)

    bool ok;
};

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

// Swallow and report whatever is sitting in the error queue. Returns the first
// error found (0 = clean) so a caller can log one line instead of a loop. Bounded,
// because a context that reports an error forever must not hang the game in here.
// Defined ahead of the loader so load-time code shares the one drain, and free of an
// `ok` guard for the same reason: every other caller already checked `ok` itself.
static GLenum FeedGlDrainErrors(FeedGl *gl)
{
    GLenum first = GL_NO_ERROR;
    for (int i = 0; i < 32; ++i)
    {
        const GLenum e = gl->GetError();
        if (e == GL_NO_ERROR) break;
        if (first == GL_NO_ERROR) first = e;
    }
    return first;
}

// Read the context's advertised extension set ONCE, both ways, and answer the only
// question the loader asks of it: which of `names` are listed. `found` receives one
// flag per name.
//
// Both enumerations are read and unioned, never short-circuited. NVIDIA hands even
// ancient games a 4.6 compatibility context, so either form may be the one that
// answers; and when ReShade is installed for OpenGL it IS the local opengl32.dll, and
// a proxy that answers one form need not answer the other. Letting whichever form
// spoke first be authoritative turns a half-answer into "this GPU cannot do interop",
// which disables the feed outright.
//
// gl->diag records how each enumeration itself behaved, because a bare
// "GL_EXT_memory_object missing" cannot tell a GPU that lacks the interop apart from a
// query that answered nothing -- and on hardware that plainly has it, the second is
// what happens. `listed` is how many of `names` each form carried, counted in this
// same pass, so it is the matcher's own verdict rather than a second guess at it.
static void FeedGlSurveyExtensions(FeedGl *gl, const char *const *names, int n, bool *found)
{
    for (int k = 0; k < n; ++k) found[k] = false;

    // The game and ReShade share this context: an error left behind by either would
    // otherwise be reported as ours and read as evidence about a query that succeeded.
    FeedGlDrainErrors(gl);

    GLint count = 0;
    GLenum err_i = GL_NO_ERROR;
    int listed_i = 0;
    if (gl->GetStringi != nullptr)
    {
        gl->GetIntegerv(GL_NUM_EXTENSIONS, &count);
        err_i = FeedGlDrainErrors(gl);                  // GL_INVALID_ENUM pre-3.0
        for (GLint i = 0; i < count; ++i)
        {
            const GLubyte *e = gl->GetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
            if (e == nullptr) continue;
            for (int k = 0; k < n; ++k)
                if (!found[k] && strcmp(reinterpret_cast<const char *>(e), names[k]) == 0)
                { found[k] = true; ++listed_i; }
        }
    }

    const GLubyte *all = gl->GetString(GL_EXTENSIONS);
    const GLenum err_s = FeedGlDrainErrors(gl);         // GL_INVALID_ENUM in a core profile
    const char *const s = reinterpret_cast<const char *>(all);
    int listed_s = 0;
    if (s != nullptr)
        for (int k = 0; k < n; ++k)
        {
            const size_t len = strlen(names[k]);
            for (const char *p = strstr(s, names[k]); p != nullptr; p = strstr(p + 1, names[k]))
                if ((p == s || p[-1] == ' ') && (p[len] == ' ' || p[len] == '\0'))
                { found[k] = true; ++listed_s; break; }
        }

    sprintf_s(gl->diag,
              "glGetStringi=%s GL_NUM_EXTENSIONS=%d err=0x%04X listed=%d/%d | "
              "glGetString(GL_EXTENSIONS)=%s len=%u err=0x%04X listed=%d/%d",
              gl->GetStringi != nullptr ? "resolved" : "NULL", static_cast<int>(count),
              static_cast<unsigned>(err_i), listed_i, n,
              s != nullptr ? "ok" : "NULL",
              s != nullptr ? static_cast<unsigned>(strlen(s)) : 0u,
              static_cast<unsigned>(err_s), listed_s, n);
}

// Does this context actually do external-object interop, whatever the extension string
// claims? NVIDIA's per-application profiles cap the string reported to old engines --
// id Tech 3 and its descendants copy glGetString(GL_EXTENSIONS) into a fixed 4 KB
// buffer, so the driver truncates the advertised set for those executables and
// GL_EXT_memory_object falls off the end. The capability is untouched; only the
// advertisement is. Asking the driver to hand back a live memory object and a live
// semaphore settles it -- and settles the stub worry with it, which is why a resolved
// entry point was never the gate on its own: a stub cannot produce a non-zero object
// name and leave GL_NO_ERROR behind.
static bool FeedGlProbeInterop(FeedGl *gl)
{
    FeedGlDrainErrors(gl);

    GLuint mem = 0;
    gl->CreateMemoryObjectsEXT(1, &mem);
    const bool mem_ok = FeedGlDrainErrors(gl) == GL_NO_ERROR && mem != 0;
    if (mem != 0) gl->DeleteMemoryObjectsEXT(1, &mem);

    GLuint sem = 0;
    gl->GenSemaphoresEXT(1, &sem);
    const bool sem_ok = FeedGlDrainErrors(gl) == GL_NO_ERROR && sem != 0;
    if (sem != 0) gl->DeleteSemaphoresEXT(1, &sem);

    FeedGlDrainErrors(gl);
    return mem_ok && sem_ok;
}

// Resolve everything on the context current RIGHT NOW. Returns false (with
// gl->missing naming the culprit) if the interop extensions are absent or any
// entry point is missing. The gate is the extension string first and a live probe
// second -- never a non-null ProcAddress on its own, because drivers happily return
// stubs for unsupported entries.
static bool FeedGlLoad(FeedGl *gl)
{
    *gl = {};

    gl->lib = GetModuleHandleW(L"opengl32.dll");
    if (gl->lib == nullptr) { strcpy_s(gl->missing, "opengl32.dll is not loaded in this process"); return false; }

    gl->wglGetProcAddress    = reinterpret_cast<PFN_wglGetProcAddress_>(GetProcAddress(gl->lib, "wglGetProcAddress"));
    gl->wglGetCurrentContext = reinterpret_cast<PFN_wglGetCurrentContext_>(GetProcAddress(gl->lib, "wglGetCurrentContext"));
    if (gl->wglGetProcAddress == nullptr || gl->wglGetCurrentContext == nullptr)
    { strcpy_s(gl->missing, "wglGetProcAddress/wglGetCurrentContext"); return false; }
    if (gl->wglGetCurrentContext() == nullptr)
    { strcpy_s(gl->missing, "no GL context is current on this thread"); return false; }

    // GL 1.1 entries are exported by opengl32.dll itself; everything newer only
    // exists through wglGetProcAddress, and only for the current context.
    #define FEED_GL_CORE(member, name) \
        gl->member = reinterpret_cast<PFN_gl##member##_>(GetProcAddress(gl->lib, name)); \
        if (gl->member == nullptr) { strcpy_s(gl->missing, name); return false; }
    FEED_GL_CORE(GetIntegerv,    "glGetIntegerv")
    FEED_GL_CORE(GetString,      "glGetString")
    FEED_GL_CORE(GetError,       "glGetError")
    FEED_GL_CORE(GenTextures,    "glGenTextures")
    FEED_GL_CORE(DeleteTextures, "glDeleteTextures")
    FEED_GL_CORE(BindTexture,    "glBindTexture")
    FEED_GL_CORE(Flush,          "glFlush")
    FEED_GL_CORE(Finish,         "glFinish")
    FEED_GL_CORE(Enable,         "glEnable")
    FEED_GL_CORE(Disable,        "glDisable")
    FEED_GL_CORE(IsEnabled,      "glIsEnabled")
    FEED_GL_CORE(ReadBuffer,     "glReadBuffer")
    FEED_GL_CORE(DrawBuffer,     "glDrawBuffer")
    #undef FEED_GL_CORE

    gl->GetStringi = reinterpret_cast<PFN_glGetStringi_>(gl->wglGetProcAddress("glGetStringi"));

    if (const GLubyte *r = gl->GetString(GL_RENDERER)) strncpy_s(gl->renderer, reinterpret_cast<const char *>(r), _TRUNCATE);
    if (const GLubyte *v = gl->GetString(GL_VERSION))  strncpy_s(gl->version,  reinterpret_cast<const char *>(v), _TRUNCATE);

    // The gate. Failing it means this frame is not being rendered on an NVIDIA GPU
    // (wrong GPU on a hybrid laptop, or non-NVIDIA hardware) -- DLSS could not run
    // there anyway, so the caller disables the feed rather than falling back.
    enum { kMemObj, kMemObjWin32, kSem, kSemWin32, kCopyImageArb, kCopyImageExt, kExtCount };
    static const char *const kExt[kExtCount] = {
        "GL_EXT_memory_object", "GL_EXT_memory_object_win32",
        "GL_EXT_semaphore",     "GL_EXT_semaphore_win32",
        "GL_ARB_copy_image",    "GL_EXT_copy_image",
    };
    bool have[kExtCount];
    FeedGlSurveyExtensions(gl, kExt, kExtCount, have);

    const char *unadvertised = nullptr;
    for (int k = kMemObj; k <= kSemWin32; ++k)
        if (!have[k]) { unadvertised = kExt[k]; break; }

    // glCopyImageSubData is core since 4.3; on anything older it needs an extension.
    // ReShade's own OpenGL minimum is 4.3, so this is belt and braces.
    if (atoi(gl->version) < 4 || (atoi(gl->version) == 4 && (strlen(gl->version) < 3 || gl->version[2] < '3')))
        if (!have[kCopyImageArb] && !have[kCopyImageExt])
        { strcpy_s(gl->missing, "GL_ARB_copy_image (and the context is below OpenGL 4.3)"); return false; }

    // An entry point that will not resolve while the extension was also unadvertised is
    // the extension genuinely being absent -- report it as such, so the wrong-GPU case
    // reads the same whether the driver hands back stubs or nothing at all.
    #define FEED_GL_EXT(member, name) \
        gl->member = reinterpret_cast<PFN_gl##member##_>(gl->wglGetProcAddress(name)); \
        if (gl->member == nullptr) { strcpy_s(gl->missing, unadvertised != nullptr ? unadvertised : name); return false; }
    FEED_GL_EXT(GenFramebuffers,        "glGenFramebuffers")
    FEED_GL_EXT(DeleteFramebuffers,     "glDeleteFramebuffers")
    FEED_GL_EXT(BindFramebuffer,        "glBindFramebuffer")
    FEED_GL_EXT(FramebufferTexture2D,   "glFramebufferTexture2D")
    FEED_GL_EXT(FramebufferRenderbuffer,"glFramebufferRenderbuffer")
    FEED_GL_EXT(BlitFramebuffer,        "glBlitFramebuffer")
    FEED_GL_EXT(CheckFramebufferStatus, "glCheckFramebufferStatus")
    FEED_GL_EXT(GetFramebufferAttachmentParameteriv, "glGetFramebufferAttachmentParameteriv")
    FEED_GL_EXT(CopyImageSubData,       "glCopyImageSubData")
    FEED_GL_EXT(FenceSync,              "glFenceSync")
    FEED_GL_EXT(ClientWaitSync,         "glClientWaitSync")
    FEED_GL_EXT(DeleteSync,             "glDeleteSync")

    FEED_GL_EXT(CreateMemoryObjectsEXT,     "glCreateMemoryObjectsEXT")
    FEED_GL_EXT(DeleteMemoryObjectsEXT,     "glDeleteMemoryObjectsEXT")
    FEED_GL_EXT(MemoryObjectParameterivEXT, "glMemoryObjectParameterivEXT")
    FEED_GL_EXT(TexStorageMem2DEXT,         "glTexStorageMem2DEXT")
    FEED_GL_EXT(ImportMemoryWin32HandleEXT, "glImportMemoryWin32HandleEXT")

    FEED_GL_EXT(GenSemaphoresEXT,             "glGenSemaphoresEXT")
    FEED_GL_EXT(DeleteSemaphoresEXT,          "glDeleteSemaphoresEXT")
    FEED_GL_EXT(SemaphoreParameterui64vEXT,   "glSemaphoreParameterui64vEXT")
    FEED_GL_EXT(WaitSemaphoreEXT,             "glWaitSemaphoreEXT")
    FEED_GL_EXT(SignalSemaphoreEXT,           "glSignalSemaphoreEXT")
    FEED_GL_EXT(ImportSemaphoreWin32HandleEXT,"glImportSemaphoreWin32HandleEXT")
    #undef FEED_GL_EXT

    // An unadvertised extension is a question, not a verdict: under a capped extension
    // string the interop is there and works. Only a probe that also fails is absence.
    // Asked here, after resolution, so the probe calls the same table the transport
    // will -- there is no second copy of the entry points to keep in step.
    if (unadvertised != nullptr)
    {
        if (!FeedGlProbeInterop(gl)) { strcpy_s(gl->missing, unadvertised); return false; }
        char note[128];
        sprintf_s(note, " | %s unadvertised, live probe PASSED (capped extension string)", unadvertised);
        strcat_s(gl->diag, note);
    }

    // Optional: the DSA form spares us a bind/restore during the import.
    gl->TextureStorageMem2DEXT =
        reinterpret_cast<PFN_glTextureStorageMem2DEXT_>(gl->wglGetProcAddress("glTextureStorageMem2DEXT"));

    gl->ok = true;
    return true;
}

// ---------------------------------------------------------------------------
// Imports
// ---------------------------------------------------------------------------

// Import a D3D12 shared texture (from CreateSharedHandle) as a GL_TEXTURE_2D backed
// by the same memory. `size` is the D3D12 ALLOCATION size (GetResourceAllocationInfo),
// not w*h*bpp -- it is passed in because the 32-bit stub has no D3D12 device to ask.
// Dedicated memory is mandatory for an imported committed D3D12 resource.
// The NT handle is duplicated by the driver, not consumed: the caller still owns it.
static bool FeedGlImportImage(FeedGl *gl, HANDLE d3d12_res_handle, uint64_t size,
                              GLsizei w, GLsizei h, GLenum internal_fmt,
                              GLuint *out_tex, GLuint *out_mem)
{
    *out_tex = 0;
    *out_mem = 0;
    if (!gl->ok) return false;
    FeedGlDrainErrors(gl);

    GLuint mem = 0;
    gl->CreateMemoryObjectsEXT(1, &mem);
    if (mem == 0) return false;

    const GLint dedicated = GL_TRUE;
    gl->MemoryObjectParameterivEXT(mem, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    gl->ImportMemoryWin32HandleEXT(mem, size, GL_HANDLE_TYPE_D3D12_RESOURCE_EXT, d3d12_res_handle);
    if (FeedGlDrainErrors(gl) != 0)
    {
        gl->DeleteMemoryObjectsEXT(1, &mem);
        return false;
    }

    GLuint tex = 0;
    gl->GenTextures(1, &tex);
    if (tex == 0) { gl->DeleteMemoryObjectsEXT(1, &mem); return false; }

    if (gl->TextureStorageMem2DEXT != nullptr)
    {
        // glGenTextures only reserves the name; DSA needs the object to exist, and a
        // single bind is what creates it. Cheap, and only at build time.
        GLint prev = 0;
        gl->GetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
        gl->BindTexture(GL_TEXTURE_2D, tex);
        gl->BindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev));
        gl->TextureStorageMem2DEXT(tex, 1, internal_fmt, w, h, mem, 0);
    }
    else
    {
        GLint prev = 0;
        gl->GetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
        gl->BindTexture(GL_TEXTURE_2D, tex);
        gl->TexStorageMem2DEXT(GL_TEXTURE_2D, 1, internal_fmt, w, h, mem, 0);
        gl->BindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev));
    }
    if (FeedGlDrainErrors(gl) != 0)
    {
        gl->DeleteTextures(1, &tex);
        gl->DeleteMemoryObjectsEXT(1, &mem);
        return false;
    }

    *out_tex = tex;
    *out_mem = mem;
    return true;
}

// Import a D3D12 shared fence (from CreateSharedHandle) as a GL semaphore. The
// timeline value is not part of the object: it is set per use with
// FeedGlSetSemaphoreValue before the signal or the wait.
static GLuint FeedGlImportFence(FeedGl *gl, HANDLE d3d12_fence_handle)
{
    if (!gl->ok) return 0;
    FeedGlDrainErrors(gl);
    GLuint sem = 0;
    gl->GenSemaphoresEXT(1, &sem);
    if (sem == 0) return 0;
    gl->ImportSemaphoreWin32HandleEXT(sem, GL_HANDLE_TYPE_D3D12_FENCE_EXT, d3d12_fence_handle);
    if (FeedGlDrainErrors(gl) != 0)
    {
        gl->DeleteSemaphoresEXT(1, &sem);
        return 0;
    }
    return sem;
}

static void FeedGlSetSemaphoreValue(FeedGl *gl, GLuint sem, uint64_t value)
{
    const GLuint64_feed v = value;
    gl->SemaphoreParameterui64vEXT(sem, GL_D3D12_FENCE_VALUE_EXT, &v);
}

// Signal `value` on the D3D12 fence behind `sem` once the GL commands issued so far
// have completed, releasing the listed textures. GL_LAYOUT_GENERAL_EXT throughout:
// that is the layout pairing with D3D12's ALLOW_SIMULTANEOUS_ACCESS.
static void FeedGlSignal(FeedGl *gl, GLuint sem, uint64_t value, const GLuint *textures, GLuint count)
{
    FeedGlSetSemaphoreValue(gl, sem, value);
    GLenum layouts[8];
    if (count > 8) count = 8;   // never hand the driver a layout we did not write
    for (GLuint i = 0; i < count; ++i) layouts[i] = GL_LAYOUT_GENERAL_EXT;
    gl->SignalSemaphoreEXT(sem, 0, nullptr, count, textures, layouts);
    // Without this the signal can sit in the client command buffer while D3D12's
    // GPU-side wait starves. glFlush is the cheap half of glFinish: no CPU stall.
    gl->Flush();
}

// Server-side wait: the GL command stream stalls on the GPU until the D3D12 fence
// behind `sem` reaches `value`. The CPU does not block. Note there is NO timeout in
// this extension, which is why the D3D12 side must always signal, even on failure.
static void FeedGlWait(FeedGl *gl, GLuint sem, uint64_t value, const GLuint *textures, GLuint count)
{
    FeedGlSetSemaphoreValue(gl, sem, value);
    GLenum layouts[8];
    if (count > 8) count = 8;
    for (GLuint i = 0; i < count; ++i) layouts[i] = GL_LAYOUT_GENERAL_EXT;
    gl->WaitSemaphoreEXT(sem, 0, nullptr, count, textures, layouts);
}

// Block the CPU until everything issued so far has completed, or `timeout_ms`
// elapses. This is glFinish with a deadline, and the deadline is the point: on the
// 32-bit path the GL stream may hold a glWaitSemaphoreEXT that only the helper
// process can release, and glWaitSemaphoreEXT itself has no timeout. Returns false
// on a timeout, so the caller can say so instead of hanging the game.
static bool FeedGlWaitIdle(FeedGl *gl, unsigned timeout_ms)
{
    if (!gl->ok) return true;
    GLsync_feed s = gl->FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (s == nullptr) { gl->Finish(); return true; }
    const GLenum r = gl->ClientWaitSync(s, GL_SYNC_FLUSH_COMMANDS_BIT,
                                        static_cast<GLuint64_feed>(timeout_ms) * 1000000ull);
    gl->DeleteSync(s);
    return r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED;
}

// ---------------------------------------------------------------------------
// Copies
// ---------------------------------------------------------------------------

// ReShade's OpenGL resource handles pack the object type in the upper 24 bits and
// the object name in the lower 32 (reshade_api_resource.hpp). A handle of 0 is the
// default framebuffer.
static inline GLenum FeedGlHandleType(uint64_t h) { return static_cast<GLenum>(h >> 40); }
static inline GLuint FeedGlHandleName(uint64_t h) { return static_cast<GLuint>(h & 0xFFFFFFFFull); }

// Exact-format, exact-size copy between two GL_TEXTURE_2Ds. Touches no binding
// state at all, which is why it is the per-frame workhorse for MV/Depth/Mask.
static void FeedGlCopy(FeedGl *gl, GLuint src, GLuint dst, GLsizei w, GLsizei h)
{
    gl->CopyImageSubData(src, GL_TEXTURE_2D, 0, 0, 0, 0,
                         dst, GL_TEXTURE_2D, 0, 0, 0, 0,
                         w, h, 1);
}

// Saves and restores exactly what the blit helper touches -- nothing else is
// disturbed (no programs, no texture bindings). ReShade rebinds its own state for
// the following passes anyway; this keeps us honest for the game.
struct FeedGlStateGuard
{
    FeedGl *gl;
    GLint   read_fbo, draw_fbo, read_buf, draw_buf;
    GLboolean scissor, srgb;

    explicit FeedGlStateGuard(FeedGl *g) : gl(g)
    {
        gl->GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
        gl->GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
        gl->GetIntegerv(GL_READ_BUFFER, &read_buf);
        gl->GetIntegerv(GL_DRAW_BUFFER, &draw_buf);
        scissor = gl->IsEnabled(GL_SCISSOR_TEST);
        srgb    = gl->IsEnabled(GL_FRAMEBUFFER_SRGB);
        // Blits honour the scissor box, and sRGB encode/decode on a blit would
        // re-encode frames that arrive already encoded (the GL flavour of issue #11).
        if (scissor) gl->Disable(GL_SCISSOR_TEST);
        if (srgb)    gl->Disable(GL_FRAMEBUFFER_SRGB);
        gl->GetError();
    }
    ~FeedGlStateGuard()
    {
        gl->BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read_fbo));
        gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_fbo));
        gl->ReadBuffer(static_cast<GLenum>(read_buf));
        gl->DrawBuffer(static_cast<GLenum>(draw_buf));
        if (scissor) gl->Enable(GL_SCISSOR_TEST);
        if (srgb)    gl->Enable(GL_FRAMEBUFFER_SRGB);
    }
    FeedGlStateGuard(const FeedGlStateGuard &) = delete;
    FeedGlStateGuard &operator=(const FeedGlStateGuard &) = delete;
};

// Attach one side of a blit. `rs_handle` is either a ReShade resource handle (whose
// packed type says texture vs renderbuffer) or, when `is_our_texture`, a plain
// GL_TEXTURE_2D name of ours. Handle 0 means the default framebuffer, which is not
// attachable: bind FBO 0 and select GL_BACK instead.
static bool FeedGlAttach(FeedGl *gl, GLenum target, GLuint fbo, uint64_t rs_handle, bool is_our_texture)
{
    const bool reading = (target == GL_READ_FRAMEBUFFER);
    if (!is_our_texture && rs_handle == 0)
    {
        gl->BindFramebuffer(target, 0);
        if (reading) gl->ReadBuffer(GL_BACK); else gl->DrawBuffer(GL_BACK);
        return true;
    }

    const GLenum type = is_our_texture ? GL_TEXTURE_2D : FeedGlHandleType(rs_handle);
    const GLuint name = is_our_texture ? static_cast<GLuint>(rs_handle) : FeedGlHandleName(rs_handle);

    gl->BindFramebuffer(target, fbo);
    if (type == GL_RENDERBUFFER)
        gl->FramebufferRenderbuffer(target, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, name);
    else
        gl->FramebufferTexture2D(target, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, name, 0);
    if (reading) gl->ReadBuffer(GL_COLOR_ATTACHMENT0); else gl->DrawBuffer(GL_COLOR_ATTACHMENT0);
    return gl->CheckFramebufferStatus(target) == GL_FRAMEBUFFER_COMPLETE;
}

// Blit through the two persistent FBOs. Unlike FeedGlCopy this converts formats and
// channel order, and can read what glCopyImageSubData cannot: renderbuffers and the
// default framebuffer. Call inside a FeedGlStateGuard.
static bool FeedGlBlit(FeedGl *gl, GLuint fbo_read, GLuint fbo_draw,
                       uint64_t src, bool src_is_ours, uint64_t dst, bool dst_is_ours,
                       GLsizei w, GLsizei h)
{
    if (!FeedGlAttach(gl, GL_READ_FRAMEBUFFER, fbo_read, src, src_is_ours)) return false;
    if (!FeedGlAttach(gl, GL_DRAW_FRAMEBUFFER, fbo_draw, dst, dst_is_ours)) return false;
    gl->BlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    return true;
}

// GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING of whatever is attached for reading:
// GL_LINEAR (0x2601) or GL_SRGB (0x8C40). Logged once per build so an sRGB-encoded
// technique target is visible before it manifests as a washed-out image.
static GLint FeedGlColorEncoding(FeedGl *gl, GLuint fbo, uint64_t rs_handle)
{
    GLint enc = 0;
    if (!FeedGlAttach(gl, GL_READ_FRAMEBUFFER, fbo, rs_handle, false)) return 0;
    gl->GetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER,
        rs_handle == 0 ? GL_BACK : GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &enc);
    FeedGlDrainErrors(gl);
    return enc;
}

// ---------------------------------------------------------------------------
// Formats
// ---------------------------------------------------------------------------

// DXGI_FORMAT -> GL sized internal format, sibling of FeedVkFormat. GL has no sized
// BGRA8 internal format at all, so B8G8R8A8 has no entry: the GL path never creates
// one (see GlSafeColorFormat in dlss5-feed.cpp), and the colour moves by blit, which
// is component-wise and therefore indifferent to the game surface's byte order.
static GLenum FeedGlFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:     return GL_RGBA8;
    case DXGI_FORMAT_R10G10B10A2_UNORM:  return GL_RGB10_A2;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return GL_RGBA16F;
    case DXGI_FORMAT_R11G11B10_FLOAT:    return GL_R11F_G11F_B10F;
    case DXGI_FORMAT_R32_FLOAT:          return GL_R32F;
    case DXGI_FORMAT_R16G16_FLOAT:       return GL_RG16F;
    case DXGI_FORMAT_R8_UNORM:           return GL_R8;
    default:                             return 0;
    }
}
