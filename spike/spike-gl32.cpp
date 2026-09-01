// Phase-0 spike for the 32-bit OpenGL path (PLAN-OPENGL §5, design A): a 32-bit
// OpenGL process imports a texture and two fences that a 64-bit D3D12 process
// created and duplicated in, then answers on the second fence.
//
// This settles the two open questions design A rests on:
//   * are GL_EXT_memory_object_win32 / GL_EXT_semaphore_win32 present in an x86
//     process on NVIDIA's driver?
//   * does a D3D12 -> GL import work CROSS-PROCESS (only single-process interop is
//     the well-trodden case)?
//
// Run spike-gl64.exe --serve first, then this. The GL half is src/feed_gl.h
// verbatim, compiled x86 -- the very same code dlss5-feed.addon32 uses.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgiformat.h>
#include <cstdio>
#include <cstdint>
#include <cstdlib>

#include "../src/feed_gl.h"

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

static const char *kPipeName = "\\\\.\\pipe\\dlss5-feed-spike-gl";
static const BYTE  kPatternA = 0xAB;

#pragma pack(push, 1)
struct SpikeGlShare
{
    uint64_t tex, tex_size, fence_in, fence_out;
    uint32_t width, height;
};
#pragma pack(pop)

typedef void (APIENTRY *PFN_glGetTexImage_)(GLenum, GLint, GLenum, GLenum, void *);
typedef void (APIENTRY *PFN_glClearColor_)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *PFN_glClear_)(GLbitfield);

int main()
{
    printf("spike-gl32 (pid %lu, %zu-bit)\n", GetCurrentProcessId(), sizeof(void *) * 8);

    // A hidden window with a GL context, the same shape as the 64-bit spike's.
    WNDCLASSA wc = {};
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "dlss5-feed-spike-gl32";
    wc.style         = CS_OWNDC;
    RegisterClassA(&wc);
    HWND wnd = CreateWindowExA(0, wc.lpszClassName, "spike", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                               nullptr, nullptr, wc.hInstance, nullptr);
    if (wnd == nullptr) { printf("CLIENT FAIL: CreateWindow -> %lu\n", GetLastError()); return 1; }
    HDC dc = GetDC(wnd);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize      = sizeof(pfd);
    pfd.nVersion   = 1;
    pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    const int pf = ChoosePixelFormat(dc, &pfd);
    if (pf == 0 || !SetPixelFormat(dc, pf, &pfd)) { printf("CLIENT FAIL: SetPixelFormat -> %lu\n", GetLastError()); return 1; }
    HGLRC rc = wglCreateContext(dc);
    if (rc == nullptr || !wglMakeCurrent(dc, rc)) { printf("CLIENT FAIL: wglMakeCurrent -> %lu\n", GetLastError()); return 1; }

    FeedGl gl;
    if (!FeedGlLoad(&gl))
    {
        printf("CLIENT FAIL: the OpenGL interop is unavailable in this 32-bit process: %s\n", gl.missing);
        printf("             renderer=\"%s\" version=\"%s\"\n", gl.renderer, gl.version);
        printf("             extension query: %s\n", gl.diag);
        printf("             (this is exactly the x86 parity question design A depends on)\n");
        return 1;
    }
    printf("gl: renderer=\"%s\" version=\"%s\" (interop extensions present in x86)\n", gl.renderer, gl.version);
    // Printed on the pass too: the gate can be carried by the live probe when a driver
    // caps the extension string, and a spike that only says PASS cannot tell that apart
    // from the matcher having found the extensions itself.
    printf("gl: extension query: %s\n", gl.diag);

    HMODULE m = GetModuleHandleW(L"opengl32.dll");
    auto GetTexImage = reinterpret_cast<PFN_glGetTexImage_>(GetProcAddress(m, "glGetTexImage"));
    auto ClearColor  = reinterpret_cast<PFN_glClearColor_>(GetProcAddress(m, "glClearColor"));
    auto Clear       = reinterpret_cast<PFN_glClear_>(GetProcAddress(m, "glClear"));
    if (!GetTexImage || !ClearColor || !Clear) { printf("CLIENT FAIL: glGetTexImage/glClear missing\n"); return 1; }

    HANDLE pipe = CreateFileA(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) { printf("CLIENT FAIL: the host pipe is not there (run spike-gl64.exe --serve first)\n"); return 1; }
    DWORD pid = GetCurrentProcessId(), put = 0, got = 0;
    WriteFile(pipe, &pid, sizeof(pid), &put, nullptr);

    SpikeGlShare s = {};
    if (!ReadFile(pipe, &s, sizeof(s), &got, nullptr) || got != sizeof(s))
    { printf("CLIENT FAIL: receiving the handles\n"); return 1; }
    printf("client: got texture %ux%u (%llu bytes) + 2 fences, duplicated in by the host\n",
           s.width, s.height, (unsigned long long)s.tex_size);

    const GLuint sem_in  = FeedGlImportFence(&gl, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(s.fence_in)));
    const GLuint sem_out = FeedGlImportFence(&gl, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(s.fence_out)));
    if (sem_in == 0 || sem_out == 0)
    { printf("CLIENT FAIL: cross-process D3D12 fence -> GL semaphore import (in=%u out=%u)\n", sem_in, sem_out); return 1; }

    GLuint tex = 0, mem = 0;
    if (!FeedGlImportImage(&gl, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(s.tex)), s.tex_size,
                           static_cast<GLsizei>(s.width), static_cast<GLsizei>(s.height), GL_RGBA8, &tex, &mem))
    { printf("CLIENT FAIL: cross-process D3D12 texture -> GL memory-object import (GL error 0x%04X)\n", FeedGlDrainErrors(&gl)); return 1; }
    printf("client: imported everything cross-process (GL texture %u)\n", tex);

    BYTE go = 0;
    if (!ReadFile(pipe, &go, 1, &got, nullptr) || got != 1) { printf("CLIENT FAIL: the host never released us\n"); return 1; }

    FeedGlWait(&gl, sem_in, 1, &tex, 1);
    gl.Finish();
    if (const GLenum e = FeedGlDrainErrors(&gl)) { printf("CLIENT FAIL: GL error 0x%04X after the semaphore wait\n", e); return 1; }

    const size_t bytes = static_cast<size_t>(s.width) * s.height * 4;
    BYTE *pixels = static_cast<BYTE *>(calloc(1, bytes));
    if (pixels == nullptr) return 1;
    gl.BindTexture(GL_TEXTURE_2D, tex);
    GetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    gl.BindTexture(GL_TEXTURE_2D, 0);
    if (const GLenum e = FeedGlDrainErrors(&gl)) { printf("CLIENT FAIL: glGetTexImage -> GL error 0x%04X\n", e); return 1; }
    for (size_t i = 0; i < bytes; ++i)
        if (pixels[i] != kPatternA)
        {
            printf("CLIENT FAIL: wrong bytes at offset %zu (%02X %02X %02X %02X, expected %02X everywhere)\n",
                   i, pixels[0], pixels[1], pixels[2], pixels[3], kPatternA);
            return 1;
        }
    free(pixels);
    printf("client: PASS -- the 64-bit process's pattern 0x%02X arrived intact\n", kPatternA);

    GLuint fbo = 0;
    gl.GenFramebuffers(1, &fbo);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    gl.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    gl.DrawBuffer(GL_COLOR_ATTACHMENT0);
    if (gl.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    { printf("CLIENT FAIL: the imported texture is not framebuffer-attachable\n"); return 1; }
    ClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    Clear(GL_COLOR_BUFFER_BIT);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    FeedGlSignal(&gl, sem_out, 1, &tex, 1);
    gl.Finish();
    if (const GLenum e = FeedGlDrainErrors(&gl)) { printf("CLIENT FAIL: GL error 0x%04X while writing back\n", e); return 1; }

    printf("CLIENT PASS: 32-bit OpenGL imported a 64-bit D3D12 texture + fences cross-process and answered on the second fence.\n");
    printf("(the host prints the verdict on the return leg)\n");
    Sleep(2000);   // let the host finish its readback before our handles go away
    return 0;
}
