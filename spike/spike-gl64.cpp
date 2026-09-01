// Phase-0 spike for the OpenGL transport (PLAN-OPENGL §2): D3D12 creates, OpenGL
// imports, and the frame counter crosses on a shared D3D12 fence in both directions.
//
// Run with no arguments for the 64-bit in-process test, which is the exact mechanism
// dlss5-feed.addon64 uses in an OpenGL game:
//
//   D3D12 writes pattern A into a shared texture, Signal(fence_in, 1)
//   GL     waits fence_in >= 1 (server-side), reads the texture back, verifies A
//   GL     clears the texture to pattern B, Signal(fence_out, 1)
//   D3D12  waits fence_out >= 1, reads the texture back, verifies B
//
// Run with --serve for the 32-bit companion (spike-gl32.exe): the same round trip,
// with the GL half in another, 32-bit process whose handles this one duplicates in.
// That is design A of PLAN-OPENGL §5 -- the host creates, because GL memory objects
// are import-only and a GL process therefore cannot export.
//
// The GL side goes through src/feed_gl.h verbatim, so a PASS here is a PASS for the
// header the add-on ships.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <cstdint>

#include "../src/feed_gl.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

static const char *kPipeName = "\\\\.\\pipe\\dlss5-feed-spike-gl";
static const UINT  kSize     = 64;
static const BYTE  kPatternA = 0xAB;   // written by D3D12
// pattern B is written by GL as a clear colour: opaque green, i.e. 00 FF 00 FF.

#pragma pack(push, 1)
struct SpikeGlShare
{
    uint64_t tex, tex_size, fence_in, fence_out;
    uint32_t width, height;
};
#pragma pack(pop)

#define CHECK(hr, what) \
    if (FAILED(hr)) { printf("FAIL: %s -> 0x%08lX\n", what, (unsigned long)(hr)); return 1; }

// ---------------------------------------------------------------------------
// A hidden window with a GL context on it -- the smallest thing a driver will
// hand real GL extensions to.
// ---------------------------------------------------------------------------

struct GlWindow { HWND wnd; HDC dc; HGLRC rc; };

static bool MakeGlWindow(GlWindow *out)
{
    *out = {};
    WNDCLASSA wc = {};
    wc.lpfnWndProc   = DefWindowProcA;
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.lpszClassName = "dlss5-feed-spike-gl";
    wc.style         = CS_OWNDC;
    RegisterClassA(&wc);

    out->wnd = CreateWindowExA(0, wc.lpszClassName, "spike", WS_OVERLAPPEDWINDOW,
                               0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (out->wnd == nullptr) { printf("FAIL: CreateWindow -> %lu\n", GetLastError()); return false; }
    out->dc = GetDC(out->wnd);

    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize      = sizeof(pfd);
    pfd.nVersion   = 1;
    pfd.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    const int pf = ChoosePixelFormat(out->dc, &pfd);
    if (pf == 0 || !SetPixelFormat(out->dc, pf, &pfd)) { printf("FAIL: SetPixelFormat -> %lu\n", GetLastError()); return false; }

    out->rc = wglCreateContext(out->dc);
    if (out->rc == nullptr || !wglMakeCurrent(out->dc, out->rc))
    { printf("FAIL: wglCreateContext/MakeCurrent -> %lu\n", GetLastError()); return false; }
    return true;
}

// GL 1.1 entries the spike needs but the add-on does not, so they are not in FeedGl.
typedef void (APIENTRY *PFN_glGetTexImage_)(GLenum, GLint, GLenum, GLenum, void *);
typedef void (APIENTRY *PFN_glClearColor_)(GLfloat, GLfloat, GLfloat, GLfloat);
typedef void (APIENTRY *PFN_glClear_)(GLbitfield);

struct GlExtra
{
    PFN_glGetTexImage_ GetTexImage;
    PFN_glClearColor_  ClearColor;
    PFN_glClear_       Clear;
};

static bool LoadGlExtra(GlExtra *x)
{
    HMODULE m = GetModuleHandleW(L"opengl32.dll");
    x->GetTexImage = reinterpret_cast<PFN_glGetTexImage_>(GetProcAddress(m, "glGetTexImage"));
    x->ClearColor  = reinterpret_cast<PFN_glClearColor_>(GetProcAddress(m, "glClearColor"));
    x->Clear       = reinterpret_cast<PFN_glClear_>(GetProcAddress(m, "glClear"));
    return x->GetTexImage && x->ClearColor && x->Clear;
}

// ---------------------------------------------------------------------------
// The GL half of the round trip, shared by the in-process test and the 32-bit
// client (which compiles its own copy of the same steps -- see spike-gl32.cpp).
// ---------------------------------------------------------------------------

static bool GlVerifyAndAnswer(FeedGl *gl, GlExtra *x, GLuint tex, GLuint sem_in, GLuint sem_out,
                              GLuint fbo, UINT w, UINT h, uint64_t n)
{
    // Server-side wait for the D3D12 write, then force it through so the CPU read below
    // sees it. glFinish is the spike's shortcut; the add-on never needs one.
    FeedGlWait(gl, sem_in, n, &tex, 1);
    gl->Finish();
    if (const GLenum e = FeedGlDrainErrors(gl)) { printf("FAIL: GL error 0x%04X after the semaphore wait\n", e); return false; }
    printf("  gl: waited the shared fence to %llu\n", (unsigned long long)n);

    BYTE *pixels = static_cast<BYTE *>(malloc(static_cast<size_t>(w) * h * 4));
    if (pixels == nullptr) return false;
    memset(pixels, 0, static_cast<size_t>(w) * h * 4);
    gl->BindTexture(GL_TEXTURE_2D, tex);
    x->GetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    gl->BindTexture(GL_TEXTURE_2D, 0);
    if (const GLenum e = FeedGlDrainErrors(gl)) { printf("FAIL: glGetTexImage -> GL error 0x%04X\n", e); free(pixels); return false; }

    int bad = -1;
    for (size_t i = 0; i < static_cast<size_t>(w) * h * 4; ++i)
        if (pixels[i] != kPatternA) { bad = static_cast<int>(i); break; }
    if (bad >= 0)
    {
        printf("FAIL: GL read the wrong bytes at offset %d (%02X %02X %02X %02X, expected %02X everywhere)\n",
               bad, pixels[0], pixels[1], pixels[2], pixels[3], kPatternA);
        free(pixels);
        return false;
    }
    free(pixels);
    printf("  gl: PASS -- D3D12's pattern 0x%02X arrived intact through the imported texture\n", kPatternA);

    // Answer with pattern B, then release the texture back to D3D12.
    gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    gl->FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    gl->DrawBuffer(GL_COLOR_ATTACHMENT0);
    if (gl->CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    { printf("FAIL: the imported texture is not framebuffer-attachable\n"); return false; }
    x->ClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    x->Clear(GL_COLOR_BUFFER_BIT);
    gl->BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    FeedGlSignal(gl, sem_out, n, &tex, 1);
    if (const GLenum e = FeedGlDrainErrors(gl)) { printf("FAIL: GL error 0x%04X while writing back\n", e); return false; }
    printf("  gl: wrote opaque green and signalled the second fence to %llu\n", (unsigned long long)n);
    return true;
}

// ---------------------------------------------------------------------------
// D3D12 side
// ---------------------------------------------------------------------------

struct D12
{
    ID3D12Device              *dev;
    ID3D12CommandQueue        *queue;
    ID3D12CommandAllocator    *alloc;
    ID3D12GraphicsCommandList *list;
    ID3D12Fence               *fence_in, *fence_out, *fence_local;
    HANDLE                     h_in, h_out, h_tex, ev;
    ID3D12Resource            *tex;
    D3D12_RESOURCE_DESC        rd;
    UINT64                     alloc_size;
    UINT64                     local_val;
};

static int MakeD12(D12 *d)
{
    *d = {};
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void **>(&d->dev));
    CHECK(hr, "D3D12CreateDevice");

    D3D12_COMMAND_QUEUE_DESC qd = {};
    d->dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&d->queue));
    d->dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void **>(&d->alloc));
    d->dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d->alloc, nullptr, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&d->list));
    if (d->queue == nullptr || d->alloc == nullptr || d->list == nullptr) { printf("FAIL: D3D12 queue/list\n"); return 1; }
    d->list->Close();

    hr = d->dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&d->fence_in));
    CHECK(hr, "CreateFence(in, shared)");
    hr = d->dev->CreateSharedHandle(d->fence_in, nullptr, GENERIC_ALL, nullptr, &d->h_in);
    CHECK(hr, "CreateSharedHandle(fence in)");
    hr = d->dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&d->fence_out));
    CHECK(hr, "CreateFence(out, shared)");
    hr = d->dev->CreateSharedHandle(d->fence_out, nullptr, GENERIC_ALL, nullptr, &d->h_out);
    CHECK(hr, "CreateSharedHandle(fence out)");
    hr = d->dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&d->fence_local));
    CHECK(hr, "CreateFence(local)");
    d->ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // The shared texture, exactly as MakeSharedTexGl creates it.
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    d->rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d->rd.Width            = kSize;
    d->rd.Height           = kSize;
    d->rd.DepthOrArraySize = 1;
    d->rd.MipLevels        = 1;
    d->rd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    d->rd.SampleDesc.Count = 1;
    d->rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d->rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr = d->dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &d->rd, D3D12_RESOURCE_STATE_COMMON,
                                         nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&d->tex));
    CHECK(hr, "CreateCommittedResource(shared texture)");
    hr = d->dev->CreateSharedHandle(d->tex, nullptr, GENERIC_ALL, nullptr, &d->h_tex);
    CHECK(hr, "CreateSharedHandle(texture)");
    d->alloc_size = d->dev->GetResourceAllocationInfo(0, 1, &d->rd).SizeInBytes;
    printf("d3d12: %ux%u R8G8B8A8_UNORM shared, allocation %llu bytes\n",
           kSize, kSize, (unsigned long long)d->alloc_size);
    return 0;
}

// Fill the shared texture with kPatternA and Signal(fence_in, n).
static int D12WritePattern(D12 *d, uint64_t n)
{
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 total = 0;
    d->dev->GetCopyableFootprints(&d->rd, 0, 1, 0, &fp, nullptr, nullptr, &total);

    D3D12_HEAP_PROPERTIES uhp = {};
    uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = total;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *up = nullptr;
    HRESULT hr = d->dev->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&up));
    CHECK(hr, "upload buffer");
    BYTE *p = nullptr;
    hr = up->Map(0, nullptr, reinterpret_cast<void **>(&p));
    CHECK(hr, "Map(upload)");
    memset(p, kPatternA, static_cast<size_t>(total));
    up->Unmap(0, nullptr);

    d->alloc->Reset();
    d->list->Reset(d->alloc, nullptr);
    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = up;  src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
    dst.pResource = d->tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d->list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    d->list->Close();
    ID3D12CommandList *ls[] = { d->list };
    d->queue->ExecuteCommandLists(1, ls);
    d->queue->Signal(d->fence_in, n);

    // The upload buffer must outlive the copy; the spike is not in a hurry.
    d->queue->Signal(d->fence_local, ++d->local_val);
    d->fence_local->SetEventOnCompletion(d->local_val, d->ev);
    WaitForSingleObject(d->ev, 5000);
    up->Release();
    printf("d3d12: wrote 0x%02X everywhere and signalled the first fence to %llu\n", kPatternA, (unsigned long long)n);
    return 0;
}

// Wait for fence_out >= n, read the texture back and check GL's answer.
static int D12VerifyAnswer(D12 *d, uint64_t n)
{
    if (d->fence_out->GetCompletedValue() < n)
    {
        d->fence_out->SetEventOnCompletion(n, d->ev);
        if (WaitForSingleObject(d->ev, 15000) != WAIT_OBJECT_0)
        { printf("FAIL: the GL side never signalled the second fence\n"); return 1; }
    }
    printf("d3d12: the second fence reached %llu (signalled from OpenGL)\n", (unsigned long long)n);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 total = 0;
    d->dev->GetCopyableFootprints(&d->rd, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES rhp = {};
    rhp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width            = total;
    bd.Height           = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels        = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *rb = nullptr;
    HRESULT hr = d->dev->CreateCommittedResource(&rhp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&rb));
    CHECK(hr, "readback buffer");

    d->alloc->Reset();
    d->list->Reset(d->alloc, nullptr);
    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = d->tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    d->list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    d->list->Close();
    ID3D12CommandList *ls[] = { d->list };
    d->queue->Wait(d->fence_out, n);
    d->queue->ExecuteCommandLists(1, ls);
    d->queue->Signal(d->fence_local, ++d->local_val);
    d->fence_local->SetEventOnCompletion(d->local_val, d->ev);
    if (WaitForSingleObject(d->ev, 5000) != WAIT_OBJECT_0) { printf("FAIL: the readback copy never completed\n"); return 1; }

    BYTE *bytes = nullptr;
    hr = rb->Map(0, nullptr, reinterpret_cast<void **>(&bytes));
    CHECK(hr, "Map(readback)");
    const BYTE want[4] = { 0x00, 0xFF, 0x00, 0xFF };
    for (UINT y = 0; y < kSize; ++y)
        for (UINT xx = 0; xx < kSize; ++xx)
            for (int c = 0; c < 4; ++c)
                if (bytes[y * fp.Footprint.RowPitch + xx * 4 + c] != want[c])
                {
                    printf("FAIL: D3D12 read the wrong bytes at (%u,%u) (%02X %02X %02X %02X, expected 00 FF 00 FF)\n",
                           xx, y, bytes[y * fp.Footprint.RowPitch + xx * 4 + 0], bytes[y * fp.Footprint.RowPitch + xx * 4 + 1],
                           bytes[y * fp.Footprint.RowPitch + xx * 4 + 2], bytes[y * fp.Footprint.RowPitch + xx * 4 + 3]);
                    return 1;
                }
    rb->Release();
    printf("d3d12: PASS -- OpenGL's write came back through the same memory\n");
    return 0;
}

// ---------------------------------------------------------------------------

static int RunInProcess()
{
    printf("spike-gl64: in-process D3D12 <-> OpenGL round trip (pid %lu)\n", GetCurrentProcessId());

    D12 d;
    if (const int rc = MakeD12(&d)) return rc;

    GlWindow gw;
    if (!MakeGlWindow(&gw)) return 1;

    FeedGl gl;
    if (!FeedGlLoad(&gl))
    {
        printf("FAIL: the OpenGL interop is unavailable: %s\n", gl.missing);
        printf("      renderer=\"%s\" version=\"%s\"\n", gl.renderer, gl.version);
        printf("      extension query: %s\n", gl.diag);
        printf("      (on a hybrid laptop, force this process onto the NVIDIA GPU)\n");
        return 1;
    }
    printf("gl: renderer=\"%s\" version=\"%s\" (interop extensions present)\n", gl.renderer, gl.version);
    // Printed on the pass too: the gate can be carried by the live probe when a driver
    // caps the extension string, and a spike that only says PASS cannot tell that apart
    // from the matcher having found the extensions itself.
    printf("gl: extension query: %s\n", gl.diag);
    GlExtra x;
    if (!LoadGlExtra(&x)) { printf("FAIL: glGetTexImage/glClear missing\n"); return 1; }

    const GLuint sem_in  = FeedGlImportFence(&gl, d.h_in);
    const GLuint sem_out = FeedGlImportFence(&gl, d.h_out);
    if (sem_in == 0 || sem_out == 0)
    { printf("FAIL: D3D12 fence -> GL semaphore import (in=%u out=%u)\n", sem_in, sem_out); return 1; }
    printf("gl: imported both D3D12 fences as semaphores\n");

    GLuint tex = 0, mem = 0;
    if (!FeedGlImportImage(&gl, d.h_tex, d.alloc_size, kSize, kSize, GL_RGBA8, &tex, &mem))
    { printf("FAIL: D3D12 texture -> GL memory-object import (GL error 0x%04X)\n", FeedGlDrainErrors(&gl)); return 1; }
    printf("gl: imported the D3D12 texture as GL texture %u\n", tex);

    GLuint fbo = 0;
    gl.GenFramebuffers(1, &fbo);

    if (const int rc = D12WritePattern(&d, 1)) return rc;
    if (!GlVerifyAndAnswer(&gl, &x, tex, sem_in, sem_out, fbo, kSize, kSize, 1)) return 1;
    if (const int rc = D12VerifyAnswer(&d, 1)) return rc;

    printf("SPIKE PASS: D3D12 created the shared texture and both fences, OpenGL imported all three, "
           "and the frame counter crossed in both directions.\n");
    return 0;
}

static int RunServe()
{
    printf("spike-gl64 --serve: D3D12 host for the 32-bit GL client (pid %lu)\n", GetCurrentProcessId());

    D12 d;
    if (const int rc = MakeD12(&d)) return rc;

    HANDLE pipe = CreateNamedPipeA(kPipeName, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                   1, 256, 256, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) { printf("FAIL: CreateNamedPipe -> %lu\n", GetLastError()); return 1; }
    printf("host: waiting for spike-gl32.exe on %s ...\n", kPipeName);
    if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != ERROR_PIPE_CONNECTED)
    { printf("FAIL: ConnectNamedPipe -> %lu\n", GetLastError()); return 1; }

    DWORD client_pid = 0, got = 0, put = 0;
    if (!ReadFile(pipe, &client_pid, sizeof(client_pid), &got, nullptr) || got != sizeof(client_pid))
    { printf("FAIL: reading the client pid\n"); return 1; }
    HANDLE hclient = OpenProcess(PROCESS_DUP_HANDLE, FALSE, client_pid);
    if (hclient == nullptr) { printf("FAIL: OpenProcess(%lu) -> %lu\n", client_pid, GetLastError()); return 1; }
    printf("host: client pid %lu\n", client_pid);

    // Every handle is duplicated INTO the client: GL can only import.
    SpikeGlShare s = {};
    s.width  = kSize;
    s.height = kSize;
    s.tex_size = d.alloc_size;
    HANDLE r_tex = nullptr, r_in = nullptr, r_out = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), d.h_tex, hclient, &r_tex, 0, FALSE, DUPLICATE_SAME_ACCESS) ||
        !DuplicateHandle(GetCurrentProcess(), d.h_in,  hclient, &r_in,  0, FALSE, DUPLICATE_SAME_ACCESS) ||
        !DuplicateHandle(GetCurrentProcess(), d.h_out, hclient, &r_out, 0, FALSE, DUPLICATE_SAME_ACCESS))
    { printf("FAIL: DuplicateHandle into the client -> %lu\n", GetLastError()); return 1; }
    s.tex       = reinterpret_cast<uint64_t>(r_tex);
    s.fence_in  = reinterpret_cast<uint64_t>(r_in);
    s.fence_out = reinterpret_cast<uint64_t>(r_out);
    if (!WriteFile(pipe, &s, sizeof(s), &put, nullptr) || put != sizeof(s))
    { printf("FAIL: sending the handles\n"); return 1; }
    printf("host: texture + both fences duplicated into the client\n");

    if (const int rc = D12WritePattern(&d, 1)) return rc;
    BYTE go = 1;
    WriteFile(pipe, &go, 1, &put, nullptr);

    if (const int rc = D12VerifyAnswer(&d, 1)) return rc;
    printf("SPIKE PASS: 64-bit D3D12 created the shared texture and fences, a 32-bit OpenGL process "
           "imported them cross-process, and the frame counter crossed in both directions.\n");
    return 0;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i)
        if (strcmp(argv[i], "--serve") == 0) return RunServe();
    return RunInProcess();
}
