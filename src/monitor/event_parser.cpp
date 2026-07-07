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
            // Token usage — also nested under message.usage. We only pull
            // the four fields that actually drive billing / cache reuse.
            if (auto uit = mit->find("usage"); uit != mit->end() && uit->is_object()) {
                auto num = [&](const char* k) -> unsigned int {
                    auto it = uit->find(k);
                    if (it == uit->end() || !it->is_number_unsigned()) return 0;
                    unsigned long long v = it->get<unsigned long long>();
                    if (v > 0xFFFFFFFFull) v = 0xFFFFFFFFull;
                    return (unsigned int)v;
                };
                e.tokInput         = num("input_tokens");
                e.tokOutput        = num("output_tokens");
                e.tokCacheRead     = num("cache_read_input_tokens");
                e.tokCacheCreation = num("cache_creation_input_tokens");
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