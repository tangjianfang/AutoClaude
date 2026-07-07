#include "event_parser.h"
#include <json.hpp>

EventSummary ParseEvent(std::string_view line) {
    EventSummary e;
    e.ts_ms = (LONGLONG)GetTickCount64();

    // Tolerate trailing junk; throw on first parse error.
    auto j = nlohmann::json::parse(line, /*cb*/ nullptr, /*allow_exceptions*/ false);
    if (!j.is_object()) return e;

    auto get_str = [&](const char* k) -> const std::string* {
        auto it = j.find(k);
        if (it == j.end() || !it->is_string()) return nullptr;
        return &it->get_ref<const std::string&>();
    };

    const std::string* type = get_str("type");
    if (!type) return e;

    if (*type == "assistant") {
        e.type = EvtType::Assistant;
        // stop_reason lives one level deeper: message.stop_reason
        if (auto mit = j.find("message"); mit != j.end() && mit->is_object()) {
            if (auto sit = mit->find("stop_reason"); sit != mit->end() && sit->is_string()) {
                e.is_end_turn = (sit->get<std::string>() == "end_turn");
            }
        }
    } else if (*type == "user") {
        e.type = EvtType::User;
    } else if (*type == "system") {
        e.type = EvtType::System;
        if (auto sit = j.find("subtype"); sit != j.end() && sit->is_string()) {
            e.is_api_error = (sit->get<std::string>() == "api_error");
        }
    } else {
        e.type = EvtType::Other;
    }
    return e;
}