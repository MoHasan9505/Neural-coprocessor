// nrcheck.exe - tests the two candidate fixes for DLSS-NR failing on the
// second GPU, with no game running.
//
// Run with no arguments. For every NVIDIA GPU as the neural GPU it runs three
// cases, each in its own fresh process (NGX keeps state for the life of a
// process, so cases must not share one):
//   case 1  CONTROL - the other GPU's device first, then the neural one.
//           What the add-on does in a game today.
//   case 2  FIX A - the neural device first, then the other, both alive.
//           The add-on creating its device before the game does.
//   case 3  FIX B - the neural device alone. A separate process.
// The summary names which fix works on this machine.
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

    // NGX writes nvngx.log and nvngx_dlssnr_<ver>.log into the data path and
    // rewrites them in every process, so without this only the last case's
    // logs would survive. Each case's pair is renamed after it finishes.
    void keep_ngx_logs(int c, UINT adapter)
    {
        const std::wstring tag = L"_case" + std::to_wstring(c) + L"_adapter" + std::to_wstring(adapter);
        const std::wstring core = g_dir + L"nvngx.log";
        if (exists(core))
            MoveFileExW(core.c_str(), (g_dir + L"nvngx" + tag + L".log").c_str(), MOVEFILE_REPLACE_EXISTING);
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((g_dir + L"nvngx_dlssnr_*.log").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do
        {
            std::wstring n = fd.cFileName;
            if (n.find(L"_case") != std::wstring::npos) continue;   // already kept
            std::wstring stem = n.substr(0, n.size() - 4);
            MoveFileExW((g_dir + n).c_str(), (g_dir + stem + tag + L".log").c_str(), MOVEFILE_REPLACE_EXISTING);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    const char *word(int code)
    {
        switch (code)
        {
        case 0:  return "PASS";
        case 1:  return "FAIL";
        case 2:  return "CRASH";
        case 3:  return "SKIPPED";
        case 4:  return "NO PARAMETERS";
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
    say("MGPU Bridge NR check 2.0 - %04u-%02u-%02u %02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);

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

    struct row { UINT idx; std::wstring name; int ctrl; int fix_a; int fix_b; };
    std::vector<row> rows;
    const bool multi = g.size() > 1;
    for (size_t i = 0; i < g.size(); ++i)
    {
        const UINT other = multi ? g[(i + 1) % g.size()].index : g[i].index;
        int c[4] = { 3, 3, 3, 3 };
        for (int k = 1; k <= 3; ++k)
        {
            if (!multi && k != 3) continue;   // one GPU: only the "alone" case exists
            say("");
            c[k] = run_child(k, g[i].index, other, snip);
            keep_ngx_logs(k, g[i].index);
        }
        rows.push_back({ g[i].index, g[i].name, c[1], c[2], c[3] });
    }

    say("");
    say("==================== SUMMARY ====================");
    for (const row &r : rows)
    {
        say("adapter[%u] %ls as the neural GPU:", r.idx, r.name.c_str());
        if (!multi)
        {
            say("  case 3 FIX B (alone):  %s", word(r.fix_b));
            say("  -> One GPU only. The two-GPU cases do not apply.");
            continue;
        }
        say("  case 1 CONTROL (other GPU first):  %s", word(r.ctrl));
        say("  case 2 FIX A   (neural GPU first): %s", word(r.fix_a));
        say("  case 3 FIX B   (neural GPU alone): %s", word(r.fix_b));

        // Only a CreateFeature that ran counts. 0 = it succeeded, 1 = it
        // returned an error. Anything else means that case did not reach
        // CreateFeature and cannot decide anything.
        if (r.ctrl == 2 || r.fix_a == 2 || r.fix_b == 2)
            say("  -> NGX crashed in at least one case. See the lines above.");
        else if (r.ctrl == 0)
            say("  -> The problem does not happen here: the current order works on this machine.");
        else if (r.ctrl != 1)
            say("  -> Inconclusive: the control case did not reach CreateFeature.");
        else if (r.fix_a == 0)
            say("  -> FIX A works: create the neural device before the game creates its own.");
        else if (r.fix_a == 1 && r.fix_b == 0)
            say("  -> FIX A fails, FIX B works: the neural stage needs its own process.");
        else if (r.fix_a == 1 && r.fix_b == 1)
            say("  -> No device order helps: this driver and nvngx_dlssnr.dll fail on this GPU.");
        else
            say("  -> Inconclusive: a fix case did not reach CreateFeature.");
    }
    say("Please attach nrcheck_report.txt and the nvngx_*_case*_adapter*.log files to your GitHub issue.");
    return 0;
}
