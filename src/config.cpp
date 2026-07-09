#include "config.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include <json.hpp>
#include <string>

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p = buf;
    auto pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : p.substr(0, pos);
}

namespace {

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string ToNarrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}

const char* ActionName(int a) {
    switch (a) {
        case 1:  return "restart";
        case 2:  return "hibernate";
        case 3:  return "lock";
        default: return "shutdown";
    }
}
int ActionFromName(const std::string& s) {
    if (s == "restart")   return 1;
    if (s == "hibernate") return 2;
    if (s == "lock")      return 3;
    return 0; // shutdown default
}

} // namespace

void Config::Load(const std::wstring& exeDir) {
    std::wstring path = exeDir + L"\\config.json";
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return;
    try {
        nlohmann::json j;
        f >> j;
        if (!j.is_object()) return;

        if (auto it = j.find("project");    it != j.end() && it->is_string())
            project = ToWide(it->get<std::string>());
        if (auto it = j.find("projectPath"); it != j.end() && it->is_string())
            projectPath = ToWide(it->get<std::string>());
        if (auto it = j.find("idleTimeoutSec"); it != j.end() && it->is_number_integer())
            idleTimeoutSec = it->get<int>();
        if (auto it = j.find("countdownSec"); it != j.end() && it->is_number_integer())
            countdownSec = it->get<int>();
        if (auto it = j.find("activeWindowSec"); it != j.end() && it->is_number_integer())
            activeWindowSec = it->get<int>();
        if (auto it = j.find("action"); it != j.end()) {
            if (it->is_string())        action = ActionFromName(it->get<std::string>());
            else if (it->is_number())   action = it->get<int>();
        }
        if (auto it = j.find("dryRun");            it != j.end() && it->is_boolean()) dryRun = it->get<bool>();
        if (auto it = j.find("autoSwitchSession"); it != j.end() && it->is_boolean()) autoSwitchSession = it->get<bool>();
        if (auto it = j.find("maxAutoTurns"); it != j.end() && it->is_number_integer()) maxAutoTurns = it->get<int>();
    } catch (const nlohmann::json::exception&) {
        // Malformed config — keep defaults.
    }
}

void Config::Save(const std::wstring& exeDir) const {
    std::wstring path = exeDir + L"\\config.json";
    std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!f) return;
    nlohmann::json j = {
        {"project",           ToNarrow(project)},
        {"projectPath",       ToNarrow(projectPath)},
        {"idleTimeoutSec",    idleTimeoutSec},
        {"countdownSec",      countdownSec},
        {"activeWindowSec",   activeWindowSec},
        {"action",            ActionName(action)},
        {"dryRun",            dryRun},
        {"autoSwitchSession", autoSwitchSession},
        {"maxAutoTurns",      maxAutoTurns},
    };
    f << j.dump(2);
}