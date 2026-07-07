#include "config.h"
#include "ui/window.h"
#include <windows.h>
#include <shellapi.h>
#include <string>

namespace {

std::wstring ArgValue(int argc, wchar_t** argv, const std::wstring& flag,
                      const std::wstring& def = L"") {
    for (int i = 1; i + 1 < argc; ++i) {
        if (_wcsicmp(argv[i], flag.c_str()) == 0) return argv[i + 1];
    }
    return def;
}

bool HasFlag(int argc, wchar_t** argv, const std::wstring& flag) {
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], flag.c_str()) == 0) return true;
    return false;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmdLine, int) {
    // Re-derive argc/argv from the real command line (not just cmdLine) so
    // --project / --dry-run flags work whether launched from a shell or not.
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    Config cfg;
    std::wstring exeDir = ExeDir();
    cfg.Load(exeDir);

    if (argv) {
        std::wstring proj = ArgValue(argc, argv, L"--project");
        if (!proj.empty()) {
            cfg.project = L"explicit";
            cfg.projectPath = proj;
        }
        // --dry-run / --live toggle dryRun. Default from config (true).
        if (HasFlag(argc, argv, L"--dry-run")) cfg.dryRun = true;
        if (HasFlag(argc, argv, L"--live"))   cfg.dryRun = false;
        LocalFree(argv);
    }

    RunApp(cfg);
    return 0;
}