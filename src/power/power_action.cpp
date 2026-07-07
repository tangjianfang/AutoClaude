#include "power_action.h"
#include <windows.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "powrprof.lib")
#include <powrprof.h>

bool EnableShutdownPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;
    TOKEN_PRIVILEGES tp{};
    LUID luid{};
    bool ok = false;
    if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &luid)) {
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        ok = GetLastError() != ERROR_NOT_ALL_ASSIGNED;
    }
    CloseHandle(token);
    return ok;
}

const wchar_t* ActionNameW(Action a) {
    switch (a) {
        case Action::Restart:  return L"restart";
        case Action::Hibernate:return L"hibernate";
        case Action::Lock:     return L"lock";
        case Action::Shutdown:
        default:               return L"shutdown";
    }
}

const wchar_t* PerformPowerAction(Action a, bool dryRun) {
    if (dryRun) return L"[dry-run] would have triggered power action";
    switch (a) {
        case Action::Shutdown:
            if (EnableShutdownPrivilege())
                InitiateSystemShutdownW(nullptr, L"Claude done", 0, TRUE, FALSE);
            return L"shutdown";
        case Action::Restart:
            if (EnableShutdownPrivilege())
                InitiateSystemShutdownW(nullptr, L"Claude done", 0, TRUE, TRUE);
            return L"restart";
        case Action::Hibernate:
            SetSuspendState(TRUE, FALSE, FALSE);  // powrprof.dll
            return L"hibernate";
        case Action::Lock:
            LockWorkStation();
            return L"lock";
    }
    return L"";
}