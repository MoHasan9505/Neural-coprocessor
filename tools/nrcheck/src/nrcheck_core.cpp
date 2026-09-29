// nrcheck_core.cpp - built as nvngx.dll_nrcheck.dll
//
// WHY THIS IS A DLL AND WHY IT HAS THIS NAME
// nvngx_dlssnr.dll checks the module that CALLS it and requires that module's
// file path to contain the substring "nvngx.dll". A caller that fails the
// check gets 0xBAD00002 FAIL_PlatformError - the same code the two reports
// show. If this check called NGX from the exe, every run would fail for that
// reason alone and the result would mean nothing. The add-on solves it by
// shipping as nvngx.dll_mgpu_bridge.addon64; this solves it the same way.
// An EXECUTABLE with that name breaks process startup, so the exe stays
// nrcheck.exe and only this DLL carries the name.
//
// WHAT IT DOES
// It tests the two candidate fixes against the add-on's current behaviour.
// One case per process, chosen by the caller (the case numbers are the
// report's):
//   case 1  CONTROL - what the add-on does today in a game: the other GPU's
//           device exists first and stays alive, then the neural device.
//           On an affected machine this is expected to FAIL.
//   case 2  FIX A, EARLY DEVICE - the neural device is created FIRST, then
//           the other GPU's device, both kept alive. This is the add-on
//           creating its device at load, before the game creates its own.
//   case 3  FIX B, SEPARATE PROCESS - the neural device alone in its process,
//           which is what a helper process would have.
// On the neural device it runs the add-on's startup probe (P1.0c) call for
// call: core Init -> core GetCapabilityParameters -> snippet Init_Ext ->
// snippet PopulateParameters_Impl -> Width/Height set -> snippet
// CreateFeature(Reserved18) at 1280x720. Same app id (0), same data path
// rule (the directory of the calling module), same sizes, same module
// preferences. Nothing is added that the add-on does not do.
//
// Shutdown1 is NOT called, for the same reason the add-on never calls it
// (V48): it has taken processes down on a clean teardown. The process exits
// right after, so the session goes with it.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "nvsdk_ngx.h"

namespace
{
    FILE *g_out = nullptr;

    void out(const char *fmt, ...)
    {
        char buf[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        fputs(buf, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        if (g_out != nullptr) { fputs(buf, g_out); fputc('\n', g_out); fflush(g_out); }
    }

    // ---- typedefs: copied from gpu1_context.cpp so the ABI is the add-on's ----
    typedef NVSDK_NGX_Result (NVSDK_CONV *pf_init)(unsigned long long, const wchar_t *,
        ID3D12Device *, const NVSDK_NGX_FeatureCommonInfo *, NVSDK_NGX_Version);
    typedef NVSDK_NGX_Result (NVSDK_CONV *pf_init_ext)(unsigned long long, const wchar_t *,
        ID3D12Device *, NVSDK_NGX_Version, const NVSDK_NGX_Parameter *);
    typedef NVSDK_NGX_Result (NVSDK_CONV *pf_get_caps)(NVSDK_NGX_Parameter **);
    typedef NVSDK_NGX_Result (NVSDK_CONV *pf_populate)(NVSDK_NGX_Parameter *);
    typedef NVSDK_NGX_Result (NVSDK_CONV *pf_create)(ID3D12GraphicsCommandList *,
        NVSDK_NGX_Feature, NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
    typedef NVSDK_NGX_Result (NVSDK_CONV *pf_release)(NVSDK_NGX_Handle *);
    typedef NVSDK_NGX_Result (NVSDK_CONV *pf_destroy)(NVSDK_NGX_Parameter *);

    const unsigned SEH_FAULT = 0xDEAD0001u;

    NVSDK_NGX_Result create_guarded(pf_create fn, ID3D12GraphicsCommandList *cl,
                                    NVSDK_NGX_Parameter *p, NVSDK_NGX_Handle **h,
                                    unsigned long *code)
    {
        __try { return fn(cl, NVSDK_NGX_Feature_Reserved18, p, h); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *code = (unsigned long)GetExceptionCode();
            return (NVSDK_NGX_Result)SEH_FAULT;
        }
    }

    const char *rname(NVSDK_NGX_Result r)
    {
        switch ((unsigned)r)
        {
        case NVSDK_NGX_Result_Success:                        return "Success";
        case NVSDK_NGX_Result_FAIL_FeatureNotSupported:       return "FAIL_FeatureNotSupported";
        case NVSDK_NGX_Result_FAIL_PlatformError:             return "FAIL_PlatformError";
        case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists:      return "FAIL_FeatureAlreadyExists";
        case NVSDK_NGX_Result_FAIL_FeatureNotFound:           return "FAIL_FeatureNotFound";
        case NVSDK_NGX_Result_FAIL_InvalidParameter:          return "FAIL_InvalidParameter";
        case NVSDK_NGX_Result_FAIL_NotInitialized:            return "FAIL_NotInitialized";
        case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature: return "FAIL_UnableToInitializeFeature";
        case NVSDK_NGX_Result_FAIL_OutOfDate:                 return "FAIL_OutOfDate";
        case NVSDK_NGX_Result_FAIL_OutOfGPUMemory:            return "FAIL_OutOfGPUMemory";
        case NVSDK_NGX_Result_FAIL_Denied:                    return "FAIL_Denied";
        case NVSDK_NGX_Result_FAIL_NotImplemented:            return "FAIL_NotImplemented";
        case SEH_FAULT:                                       return "CRASH_INSIDE_NGX";
        default:                                              return "unmapped";
        }
    }

    // NGX's own messages. The snippet's NvAPI failure line, if there is one,
    // arrives here - that line is what separates the two failure kinds.
    void NVSDK_CONV ngx_cb(const char *msg, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature)
    {
        if (msg == nullptr) return;
        char b[1600];
        snprintf(b, sizeof b, "  [NGX] %s", msg);
        size_t n = strlen(b);
        while (n > 0 && (b[n - 1] == '\n' || b[n - 1] == '\r')) b[--n] = '\0';
        out("%s", b);
    }

    IDXGIAdapter1 *adapter_at(UINT index)
    {
        IDXGIFactory1 *f = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return nullptr;
        IDXGIAdapter1 *a = nullptr;
        if (f->EnumAdapters1(index, &a) != S_OK) a = nullptr;
        f->Release();
        return a;
    }

    ID3D12Device *make_device(UINT index, const char *role)
    {
        IDXGIAdapter1 *a = adapter_at(index);
        if (a == nullptr) { out("%s device: adapter[%u] not found", role, index); return nullptr; }
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        ID3D12Device *dev = nullptr;
        HRESULT hr = D3D12CreateDevice(a, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
        out("%s device: adapter[%u] \"%ls\" luid=%08lX-%08lX D3D12CreateDevice hr=0x%08X",
            role, index, d.Description, (unsigned long)d.AdapterLuid.HighPart,
            (unsigned long)d.AdapterLuid.LowPart, (unsigned)hr);
        a->Release();
        return SUCCEEDED(hr) ? dev : nullptr;
    }

    HMODULE load_core()
    {
        HMODULE m = GetModuleHandleW(L"_nvngx.dll");
        if (m != nullptr) { out("core _nvngx.dll: already resident"); return m; }
        m = LoadLibraryW(L"_nvngx.dll");
        if (m != nullptr) { out("core _nvngx.dll: loaded by name"); return m; }
        // The registry route recorded in P0_RECORD section 09.
        wchar_t dir[MAX_PATH] = {};
        DWORD sz = sizeof dir;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore",
                         L"FullPath", RRF_RT_REG_SZ, nullptr, dir, &sz) == ERROR_SUCCESS)
        {
            wchar_t p[MAX_PATH * 2] = {};
            _snwprintf_s(p, MAX_PATH * 2, _TRUNCATE, L"%s\\_nvngx.dll", dir);
            m = LoadLibraryW(p);
            out("core _nvngx.dll: registry FullPath \"%ls\" -> %s", p, m ? "loaded" : "NOT loaded");
            return m;
        }
        out("core _nvngx.dll: NOT FOUND (no NVIDIA NGX core on this system?)");
        return nullptr;
    }

    FARPROC pick(HMODULE preferred, HMODULE other, const char *name, const char **from,
                 const char *pn, const char *on)
    {
        FARPROC p = preferred ? GetProcAddress(preferred, name) : nullptr;
        if (p != nullptr) { *from = pn; return p; }
        p = other ? GetProcAddress(other, name) : nullptr;
        *from = (p != nullptr) ? on : "missing";
        return p;
    }
}

// Returns: 0 = CreateFeature succeeded, 1 = CreateFeature returned an error,
// 2 = a crash inside NGX was caught, 3 = the check could not reach CreateFeature,
// 4 = GetCapabilityParameters failed after both Inits (the arm stops there too).
extern "C" __declspec(dllexport)
int nrcheck_run(int case_id, int neural_index, int other_index,
                const wchar_t *snippet_path, const wchar_t *log_path)
{
    if (log_path != nullptr && log_path[0] != L'\0') _wfopen_s(&g_out, log_path, L"a");

    static const char *const kName[4] = {
        "", "CONTROL (other GPU first - what the add-on does today)",
        "FIX A (neural device first, then the other GPU)",
        "FIX B (neural device alone - separate process)" };
    out("---- case %d, neural adapter[%d]: %s ----", case_id, neural_index,
        (case_id >= 1 && case_id <= 3) ? kName[case_id] : "?");

    // Both devices stay alive until the process exits, on purpose: the
    // question is which device is FIRST in a process where both are live.
    ID3D12Device *other = nullptr;
    ID3D12Device *dev = nullptr;
    if (case_id == 1)
    {
        other = make_device((UINT)other_index, "first  (other)");
        if (other == nullptr) { out("RESULT case %d: SKIPPED (no device on the other GPU)", case_id); return 3; }
        dev = make_device((UINT)neural_index, "second (neural)");
    }
    else if (case_id == 2)
    {
        dev = make_device((UINT)neural_index, "first  (neural)");
        if (dev != nullptr)
        {
            other = make_device((UINT)other_index, "second (other)");
            if (other == nullptr) { out("RESULT case %d: SKIPPED (no device on the other GPU)", case_id); return 3; }
        }
    }
    else
    {
        dev = make_device((UINT)neural_index, "only   (neural)");
    }
    if (dev == nullptr) { out("RESULT case %d: SKIPPED (no device on the neural GPU)", case_id); return 3; }

    HMODULE core = load_core();
    HMODULE snip = LoadLibraryW(snippet_path);
    out("snippet \"%ls\": %s", snippet_path, snip ? "loaded" : "NOT loaded");
    if (core == nullptr || snip == nullptr) { out("RESULT case %d: SKIPPED (modules)", case_id); return 3; }

    // Same preferences as the add-on: Init/Caps/Destroy from the core,
    // Create/Release from the snippet, snippet Init_Ext/Populate strictly
    // from the snippet.
    const char *w1, *w2, *w3, *w4, *w5;
    auto p_init   = (pf_init)    pick(core, snip, "NVSDK_NGX_D3D12_Init", &w1, "core", "snippet!FALLBACK");
    auto p_caps   = (pf_get_caps)pick(core, snip, "NVSDK_NGX_D3D12_GetCapabilityParameters", &w2, "core", "snippet!FALLBACK");
    auto p_dest   = (pf_destroy) pick(core, snip, "NVSDK_NGX_D3D12_DestroyParameters", &w3, "core", "snippet!FALLBACK");
    auto p_create = (pf_create)  pick(snip, core, "NVSDK_NGX_D3D12_CreateFeature", &w4, "snippet", "core!FALLBACK");
    auto p_rel    = (pf_release) pick(snip, core, "NVSDK_NGX_D3D12_ReleaseFeature", &w5, "snippet", "core!FALLBACK");
    auto ps_iext  = (pf_init_ext)GetProcAddress(snip, "NVSDK_NGX_D3D12_Init_Ext");
    auto ps_pop   = (pf_populate)GetProcAddress(snip, "NVSDK_NGX_D3D12_PopulateParameters_Impl");
    out("exports: Init=%s Caps=%s Destroy=%s CreateFeature=%s Release=%s snippet Init_Ext=%s Populate=%s",
        w1, w2, w3, w4, w5, ps_iext ? "yes" : "no", ps_pop ? "yes" : "no");
    if (!p_init || !p_caps || !p_create) { out("RESULT case %d: SKIPPED (missing exports)", case_id); return 3; }

    // Data path: the directory of the calling module, as the add-on does.
    wchar_t data_path[MAX_PATH] = {};
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&nrcheck_run, &self);
        GetModuleFileNameW(self, data_path, MAX_PATH);
        wchar_t *s = wcsrchr(data_path, L'\\');
        if (s != nullptr) s[1] = L'\0';
    }

    NVSDK_NGX_FeatureCommonInfo common{};
    common.LoggingInfo.LoggingCallback = ngx_cb;
    common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_VERBOSE;
    common.LoggingInfo.DisableOtherLoggingSinks = false;

    // THE ADD-ON'S ORDER, NOT A RETRY POLICY. In a game the add-on calls core
    // Init twice on the same device in one process: once in the startup probe
    // (P1.0c), and again at arm (P4.1), with the session never shut down in
    // between. The probe stops on a non-Success Init; the arm ignores its
    // Init result (V31) and goes on to GetCapabilityParameters and
    // CreateFeature. FAIL_OutOfDate is not a gate (DEBUGGING_LEDGER,
    // 2026-09-14): on the rig the arm creates both handles with it present.
    //
    // So: Init once as the probe does. If that is not Success, Init again on
    // the same device as the arm does, and carry on from there whatever it
    // returns. Two calls, because the add-on makes two. No third.
    NVSDK_NGX_Result r = p_init(0ULL, data_path, dev, &common, NVSDK_NGX_Version_API);
    out("core Init (probe order): 0x%08X (%s) app_id=0", (unsigned)r, rname(r));
    const NVSDK_NGX_Result init_r = r;
    NVSDK_NGX_Result init2_r = r;
    if (r != NVSDK_NGX_Result_Success)
    {
        r = p_init(0ULL, data_path, dev, &common, NVSDK_NGX_Version_API);
        init2_r = r;
        out("core Init (arm order, same device, session kept): 0x%08X (%s) - result ignored as the arm does (V31)",
            (unsigned)r, rname(r));
    }

    NVSDK_NGX_Parameter *params = nullptr;
    r = p_caps(&params);
    out("GetCapabilityParameters: 0x%08X (%s)", (unsigned)r, rname(r));
    if (r != NVSDK_NGX_Result_Success || params == nullptr)
    {
        // The arm returns false here too. If this line appears the check has
        // hit something the add-on would also stop on, and the two Init
        // results above are the evidence.
        out("RESULT case %d: NO PARAMETERS 0x%08X (%s) | Init probe 0x%08X (%s), Init arm 0x%08X (%s)",
            case_id, (unsigned)r, rname(r), (unsigned)init_r, rname(init_r),
            (unsigned)init2_r, rname(init2_r));
        return 4;
    }

    if (ps_iext != nullptr)
    {
        r = ps_iext(0ULL, data_path, dev, NVSDK_NGX_Version_API, params);
        out("snippet Init_Ext: 0x%08X (%s)", (unsigned)r, rname(r));
    }
    if (ps_pop != nullptr)
    {
        r = ps_pop(params);
        out("snippet PopulateParameters_Impl: 0x%08X (%s)", (unsigned)r, rname(r));
    }

    const unsigned W = 1280, H = 720;
    params->Set(NVSDK_NGX_Parameter_Width, W);
    params->Set(NVSDK_NGX_Parameter_Height, H);
    params->Set("DLSSNR.Width", W);
    params->Set("DLSSNR.Height", H);

    ID3D12CommandQueue *q = nullptr;
    ID3D12CommandAllocator *al = nullptr;
    ID3D12GraphicsCommandList *cl = nullptr;
    ID3D12Fence *fe = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) ||
        FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al))) ||
        FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&cl))) ||
        FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fe))))
    {
        out("RESULT case %d: SKIPPED (D3D12 objects)", case_id);
        return 3;
    }

    NVSDK_NGX_Handle *h = nullptr;
    unsigned long seh = 0;
    LARGE_INTEGER f{}, t0{}, t1{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    r = create_guarded(p_create, cl, params, &h, &seh);
    QueryPerformanceCounter(&t1);
    const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;

    if ((unsigned)r == SEH_FAULT)
    {
        // Do not execute a list NGX faulted while recording. Leave now.
        out("RESULT case %d: CRASH INSIDE NGX CreateFeature (exception 0x%08lX)", case_id, seh);
        return 2;
    }
    out("CreateFeature(Reserved18) %ux%u: 0x%08X (%s) handle=%p elapsed=%.0fms",
        W, H, (unsigned)r, rname(r), (void *)h, ms);

    // Drain whatever was recorded, as the add-on's probe teardown does.
    cl->Close();
    ID3D12CommandList *ls[1] = { cl };
    q->ExecuteCommandLists(1, ls);
    q->Signal(fe, 1);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ev != nullptr && fe->GetCompletedValue() < 1)
    {
        fe->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, 10000);
    }
    if (ev != nullptr) CloseHandle(ev);

    if (h != nullptr && p_rel != nullptr) p_rel(h);
    if (p_dest != nullptr) p_dest(params);

    const bool ok = (r == NVSDK_NGX_Result_Success && h != nullptr);
    out("RESULT case %d: %s 0x%08X (%s) | Init probe 0x%08X (%s), Init arm 0x%08X (%s)", case_id,
        ok ? "PASS" : "FAIL", (unsigned)r, rname(r), (unsigned)init_r, rname(init_r),
        (unsigned)init2_r, rname(init2_r));
    (void)other;   // kept alive until the process exits, on purpose
    return ok ? 0 : 1;
}
