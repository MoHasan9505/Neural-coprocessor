// nrcheck.exe - runs the DLSS-NR create check with no game running.
//
// Run with no arguments. For every NVIDIA GPU it runs two cases, each in its
// own fresh process (NGX keeps state for the life of a process, so cases must
// not share one):
//   case 1  that GPU alone
//   case 2  the other GPU's D3D12 device created first and kept alive,
//           then that GPU - the shape the add-on always has in a game
// Output goes to the console and to nrcheck_report.txt beside the exe.
//
// Internal: nrcheck.exe --case N --neural I --other J --snippet P --log L
// is how the parent starts each child. Not meant to be typed by hand.

#include <windows.h>
#include <dxgi1_4.h>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace
{
    std::wstring g_dir;        // exe directory, with trailing backslash
    std::wstring g_report;     // report path

    void say(const char *fmt, ...)
    {
        char buf[2048];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        fputs(buf, stdout); fputc('\n', stdout); fflush(stdout);
        FILE *f = nullptr;
        if (_wfopen_s(&f, g_report.c_str(), L"a") == 0 && f != nullptr)
        { fputs(buf, f); fputc('\n', f); fclose(f); }
    }

    bool exists(const std::wstring &p)
    {
        const DWORD a = GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    std::string file_version(const std::wstring &p)
    {
        DWORD h = 0, n = GetFileVersionInfoSizeW(p.c_str(), &h);
        if (n == 0) return "unknown";
        std::vector<BYTE> b(n);
        if (!GetFileVersionInfoW(p.c_str(), 0, n, b.data())) return "unknown";
        VS_FIXEDFILEINFO *fi = nullptr; UINT l = 0;
        if (!VerQueryValueW(b.data(), L"\\", (void **)&fi, &l) || fi == nullptr) return "unknown";
        char s[64];
        snprintf(s, sizeof s, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                 HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
        return s;
    }

    long long file_size(const std::wstring &p)
    {
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return -1;
        return ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    }

    struct gpu { UINT index; std::wstring name; std::string driver; LUID luid; };

    // 32.0.16.1714 -> 617.14
    std::string nv_driver(IDXGIAdapter1 *a)
    {
        LARGE_INTEGER v{};
        if (FAILED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v))) return "unknown";
        const unsigned c = HIWORD(v.LowPart), d = LOWORD(v.LowPart);
        const unsigned n = (c % 10) * 10000 + d;
        char s[64];
        snprintf(s, sizeof s, "%u.%02u (%u.%u.%u.%u)", n / 100, n % 100,
                 HIWORD(v.HighPart), LOWORD(v.HighPart), c, d);
        return s;
    }

    std::vector<gpu> nvidia_gpus()
    {
        std::vector<gpu> out;
        IDXGIFactory1 *f = nullptr;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return out;
        IDXGIAdapter1 *a = nullptr;
        for (UINT i = 0; f->EnumAdapters1(i, &a) == S_OK; ++i)
        {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            if (d.VendorId == 0x10DE && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                out.push_back({ i, d.Description, nv_driver(a), d.AdapterLuid });
            a->Release();
        }
        f->Release();
        return out;
    }

    int run_child(int c, UINT neural, UINT other, const std::wstring &snip)
    {
        wchar_t exe[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --case " + std::to_wstring(c) +
            L" --neural " + std::to_wstring(neural) + L" --other " + std::to_wstring(other) +
            L" --snippet \"" + snip + L"\" --log \"" + g_report + L"\"";
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(L'\0');
        STARTUPINFOW si{}; si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, 0, nullptr,
                            g_dir.c_str(), &si, &pi))
            return -1;
        DWORD code = 0xFFFFFFFF;
        if (WaitForSingleObject(pi.hProcess, 120000) == WAIT_TIMEOUT)
        {
            TerminateProcess(pi.hProcess, 0xDEAD);
            code = 0xDEAD;
        }
        else GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return (int)code;
    }

    const char *word(int code)
    {
        switch (code)
        {
        case 0:  return "PASS";
        case 1:  return "FAIL";
        case 2:  return "CRASH";
        case 3:  return "SKIPPED";
        case 0xDEAD: return "TIMEOUT";
        default: return "PROCESS DIED";
        }
    }

    int child_main(int argc, wchar_t **argv)
    {
        int c = 1; int n = 0; int o = 0; std::wstring snip, log;
        for (int i = 1; i + 1 < argc; ++i)
        {
            if (!wcscmp(argv[i], L"--case"))    c = _wtoi(argv[++i]);
            else if (!wcscmp(argv[i], L"--neural"))  n = _wtoi(argv[++i]);
            else if (!wcscmp(argv[i], L"--other"))   o = _wtoi(argv[++i]);
            else if (!wcscmp(argv[i], L"--snippet")) snip = argv[++i];
            else if (!wcscmp(argv[i], L"--log"))     log = argv[++i];
        }
        HMODULE m = LoadLibraryW((g_dir + L"nvngx.dll_nrcheck.dll").c_str());
        if (m == nullptr) { printf("nvngx.dll_nrcheck.dll not found beside nrcheck.exe\n"); return 3; }
        typedef int (*run_fn)(int, int, int, const wchar_t *, const wchar_t *);
        run_fn run = (run_fn)GetProcAddress(m, "nrcheck_run");
        if (run == nullptr) return 3;
        return run(c, n, o, snip.c_str(), log.c_str());
    }
}

int wmain(int argc, wchar_t **argv)
{
    wchar_t exe[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH * 2);
    g_dir = exe;
    g_dir.erase(g_dir.find_last_of(L'\\') + 1);
    g_report = g_dir + L"nrcheck_report.txt";

    for (int i = 1; i < argc; ++i)
        if (!wcscmp(argv[i], L"--case")) return child_main(argc, argv);

    std::wstring snip;
    for (int i = 1; i + 1 < argc; ++i)
        if (!wcscmp(argv[i], L"--snippet")) snip = argv[i + 1];
    if (snip.empty())
    {
        if (exists(g_dir + L"mgpu\\nvngx_dlssnr.dll")) snip = g_dir + L"mgpu\\nvngx_dlssnr.dll";
        else if (exists(g_dir + L"nvngx_dlssnr.dll")) snip = g_dir + L"nvngx_dlssnr.dll";
    }

    DeleteFileW(g_report.c_str());
    SYSTEMTIME t; GetLocalTime(&t);
    say("MGPU Bridge NR check 1.0 - %04u-%02u-%02u %02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);

    if (snip.empty())
    {
        say("nvngx_dlssnr.dll NOT FOUND. Put nrcheck.exe in the game folder beside the mgpu\\ "
            "folder, or run: nrcheck.exe --snippet \"C:\\path\\to\\nvngx_dlssnr.dll\"");
        return 3;
    }
    say("nvngx_dlssnr.dll: %ls | version %s | %lld bytes", snip.c_str(),
        file_version(snip).c_str(), file_size(snip));

    const std::vector<gpu> g = nvidia_gpus();
    for (const gpu &x : g)
        say("GPU adapter[%u] %ls | driver %s | luid %08lX-%08lX", x.index, x.name.c_str(),
            x.driver.c_str(), (unsigned long)x.luid.HighPart, (unsigned long)x.luid.LowPart);
    if (g.empty()) { say("No NVIDIA GPU found."); return 3; }

    struct row { UINT idx; std::wstring name; int c1; int c2; };
    std::vector<row> rows;
    for (size_t i = 0; i < g.size(); ++i)
    {
        const UINT other = (g.size() > 1) ? g[(i + 1) % g.size()].index : g[i].index;
        say("");
        const int c1 = run_child(1, g[i].index, other, snip);
        int c2 = 3;
        if (g.size() > 1) { say(""); c2 = run_child(2, g[i].index, other, snip); }
        rows.push_back({ g[i].index, g[i].name, c1, c2 });
    }

    say("");
    say("==================== SUMMARY ====================");
    for (const row &r : rows)
    {
        say("adapter[%u] %ls: case 1 (alone) %s | case 2 (other GPU first) %s",
            r.idx, r.name.c_str(), word(r.c1), rows.size() > 1 ? word(r.c2) : "n/a (one GPU)");
        if (r.c1 != 0)
            say("  -> This driver and nvngx_dlssnr.dll cannot create DLSS-NR on this GPU, even alone.");
        else if (rows.size() > 1 && r.c2 != 0)
            say("  -> Works alone, fails when another GPU's device exists first.");
        else
            say("  -> This GPU, driver and nvngx_dlssnr.dll can create DLSS-NR.");
    }
    say("Please attach nrcheck_report.txt to your GitHub issue.");
    return 0;
}
