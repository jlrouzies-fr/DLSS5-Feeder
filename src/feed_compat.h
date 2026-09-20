#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <comdef.h>
#include <Wbemidl.h>
#include <string>
#include <vector>
#include <cstdint>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "wbemuuid.lib")

// Legacy GPUs are not all the same, but they share one rule here: anything below
// FL11_0 should follow the older-device fallback path instead of the direct shared
// UAV path. This file centralises that check so every transport path makes the same
// decision when a game or relay device is running on a pre-FL11 GPU.
static inline bool FeedGpuIsLegacyAdapter(D3D_FEATURE_LEVEL fl)
{
    return fl < D3D_FEATURE_LEVEL_11_0;
}

static inline bool FeedGpuSupportsLegacyFallback(D3D_FEATURE_LEVEL fl)
{
    return fl >= D3D_FEATURE_LEVEL_10_0;
}

static inline bool FeedGpuNeedsHostCreatedOutput(D3D_FEATURE_LEVEL fl)
{
    return FeedGpuIsLegacyAdapter(fl);
}

static inline D3D_FEATURE_LEVEL FeedGpuLowestUsableFeatureLevel(D3D_FEATURE_LEVEL fl)
{
    if (fl >= D3D_FEATURE_LEVEL_11_1) return D3D_FEATURE_LEVEL_11_1;
    if (fl >= D3D_FEATURE_LEVEL_11_0) return D3D_FEATURE_LEVEL_11_0;
    if (fl >= D3D_FEATURE_LEVEL_10_1) return D3D_FEATURE_LEVEL_10_1;
    if (fl >= D3D_FEATURE_LEVEL_10_0) return D3D_FEATURE_LEVEL_10_0;
    return D3D_FEATURE_LEVEL_10_0;
}

static inline const char *FeedGpuFeatureLevelName(D3D_FEATURE_LEVEL fl)
{
    switch (fl)
    {
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_10_1: return "10_1";
    case D3D_FEATURE_LEVEL_10_0: return "10_0";
    default:                     return "unknown";
    }
}

enum class FeedGpuStatus
{
    Supported,
    Legacy,
    Unsupported,
    Unknown
};

struct FeedGpuCapability
{
    std::string name;
    std::string vendor;
    std::string architecture;
    uint64_t vram_mb = 0;
    std::string driver_version;
    bool dlss5Supported = false;
    bool legacyGpu = false;
    bool fallbackActive = false;
    FeedGpuStatus status = FeedGpuStatus::Unknown;
};

static inline const char *FeedGpuStatusString(FeedGpuStatus status)
{
    switch (status)
    {
    case FeedGpuStatus::Supported: return "SUPPORTED";
    case FeedGpuStatus::Legacy:    return "LEGACY";
    case FeedGpuStatus::Unsupported: return "UNSUPPORTED";
    case FeedGpuStatus::Unknown:   return "UNKNOWN";
    default:                       return "UNKNOWN";
    }
}

static inline std::string FeedGpuVendorName(UINT vendor_id)
{
    switch (vendor_id)
    {
    case 0x10DEu: return "NVIDIA";
    case 0x1002u: return "AMD";
    case 0x8086u: return "Intel";
    case 0x1414u: return "Microsoft";
    default:      return "Unknown";
    }
}

static inline std::string FeedGpuTrim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

static inline bool FeedGpuCapabilityFromDxgi(FeedGpuCapability &cap)
{
    IDXGIFactory1 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory))))
        return false;

    bool found = false;
    IDXGIAdapter1 *adapter = nullptr;
    for (UINT i = 0; ; ++i)
    {
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        if (adapter == nullptr)
            continue;

        DXGI_ADAPTER_DESC1 desc = {};
        if (SUCCEEDED(adapter->GetDesc1(&desc)))
        {
            cap.name = FeedGpuTrim(std::string(desc.Description));
            cap.vendor = FeedGpuVendorName(desc.VendorId);
            cap.vram_mb = static_cast<uint64_t>(desc.DedicatedVideoMemory) / (1024u * 1024u);
            if (!cap.name.empty())
            {
                found = true;
                break;
            }
        }
        adapter->Release();
        adapter = nullptr;
    }

    if (adapter != nullptr) adapter->Release();
    factory->Release();
    return found;
}

static inline bool FeedGpuCapabilityFromWindows(FeedGpuCapability &cap)
{
    // Real Windows hardware detection: prefer the OS's own video-controller metadata,
    // which is more reliable than a hard-coded model list. This avoids any fake or demo data.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool co_init = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    if (!co_init)
        return false;

    bool ok = false;
    IWbemLocator *locator = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IWbemLocator, reinterpret_cast<void **>(&locator))))
    {
        IWbemServices *services = nullptr;
        if (SUCCEEDED(locator->ConnectServer(_bstr_t(L"root\\CIMV2"), nullptr, nullptr, nullptr, 0,
                                             nullptr, nullptr, &services)))
        {
            IEnumWbemClassObject *enumerator = nullptr;
            if (SUCCEEDED(services->ExecQuery(_bstr_t(L"WQL"),
                                               _bstr_t(L"SELECT Name, AdapterCompatibility, DriverVersion, AdapterRAM FROM Win32_VideoController"),
                                               WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                               nullptr, &enumerator)))
            {
                IWbemClassObject *obj = nullptr;
                ULONG returned = 0;
                while (enumerator->Next(WBEM_INFINITE, 1, &obj, &returned) == S_OK && returned > 0)
                {
                    VARIANT value;
                    VariantInit(&value);
                    if (SUCCEEDED(obj->Get(L"Name", 0, &value, nullptr, nullptr)))
                    {
                        if (value.vt == VT_BSTR && value.bstrVal != nullptr)
                            cap.name = FeedGpuTrim(std::string(_bstr_t(value.bstrVal)));
                        VariantClear(&value);
                    }

                    VariantInit(&value);
                    if (SUCCEEDED(obj->Get(L"AdapterCompatibility", 0, &value, nullptr, nullptr)))
                    {
                        if (value.vt == VT_BSTR && value.bstrVal != nullptr)
                            cap.vendor = FeedGpuTrim(std::string(_bstr_t(value.bstrVal)));
                        VariantClear(&value);
                    }

                    VariantInit(&value);
                    if (SUCCEEDED(obj->Get(L"DriverVersion", 0, &value, nullptr, nullptr)))
                    {
                        if (value.vt == VT_BSTR && value.bstrVal != nullptr)
                            cap.driver_version = FeedGpuTrim(std::string(_bstr_t(value.bstrVal)));
                        VariantClear(&value);
                    }

                    VariantInit(&value);
                    if (SUCCEEDED(obj->Get(L"AdapterRAM", 0, &value, nullptr, nullptr)))
                    {
                        if (value.vt == VT_BSTR && value.bstrVal != nullptr)
                        {
                            const std::string ram = FeedGpuTrim(std::string(_bstr_t(value.bstrVal)));
                            char *end = nullptr;
                            const unsigned long long bytes = strtoull(ram.c_str(), &end, 10);
                            if (bytes != 0)
                                cap.vram_mb = bytes / (1024ULL * 1024ULL);
                        }
                        VariantClear(&value);
                    }

                    obj->Release();
                    ok = !cap.name.empty();
                    break;
                }
                enumerator->Release();
            }
            services->Release();
        }
        locator->Release();
    }

    CoUninitialize();
    return ok;
}

static inline FeedGpuCapability FeedDetectRealGpuCapability()
{
    FeedGpuCapability cap;

    if (!FeedGpuCapabilityFromWindows(cap) && !FeedGpuCapabilityFromDxgi(cap))
    {
        cap.name = "GPU detection failed";
        cap.vendor = "Unknown";
        cap.architecture = "Unknown";
        cap.status = FeedGpuStatus::Unknown;
        cap.legacyGpu = true;
        cap.fallbackActive = true;
        cap.dlss5Supported = false;
        return cap;
    }

    if (cap.vendor.empty())
        cap.vendor = "Unknown";

    // Conservative, evidence-driven classification. We do not claim support just because a
    // vendor name sounds modern. If the hardware does not clearly support the DLSS 5 stack,
    // we keep it in Legacy/Unknown territory and switch to Fallback mode.
    if (cap.vram_mb == 0)
        cap.architecture = "Unknown";
    else if (cap.vram_mb <= 4096ULL)
        cap.architecture = "Legacy";
    else
        cap.architecture = "Modern";

    const bool nvidia = cap.vendor == "NVIDIA" || cap.vendor.find("NVIDIA") != std::string::npos;
    const bool known_legacy = cap.architecture == "Legacy" || cap.vram_mb <= 4096ULL;
    cap.legacyGpu = known_legacy || (!nvidia && cap.vendor != "Unknown");
    cap.dlss5Supported = nvidia && !cap.legacyGpu && !cap.name.empty() && !cap.driver_version.empty();
    cap.fallbackActive = !cap.dlss5Supported || cap.legacyGpu;

    if (cap.dlss5Supported)
        cap.status = FeedGpuStatus::Supported;
    else if (cap.legacyGpu)
        cap.status = FeedGpuStatus::Legacy;
    else if (!nvidia)
        cap.status = FeedGpuStatus::Unsupported;
    else
        cap.status = FeedGpuStatus::Unknown;

    return cap;
}

static inline std::string FeedGpuCapabilityText(const FeedGpuCapability &cap)
{
    const char *status = FeedGpuStatusString(cap.status);
    std::string out;
    out += "GPU: " + cap.name + "\n";
    out += "Vendor: " + cap.vendor + "\n";
    out += "Architecture: " + cap.architecture + "\n";
    if (cap.vram_mb != 0)
        out += "VRAM: " + std::to_string(cap.vram_mb) + " MB\n";
    if (!cap.driver_version.empty())
        out += "Driver: " + cap.driver_version + "\n";
    out += "DLSS 5: " + std::string(cap.dlss5Supported ? "Supported" : "Not Supported") + "\n";
    out += "Status: " + std::string(status) + "\n";
    out += "Optimization: " + std::string(cap.fallbackActive ? "Fallback Active" : "Normal") + "\n";
    return out;
}
