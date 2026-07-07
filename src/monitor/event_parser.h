#pragma once
#include "../app.h"
#include <string_view>

// Parse one complete JSONL line into an EventSummary. Uses targeted string
// search (no JSON library) — we only need `type`, `subtype`, and the nested
// `message.stop_reason` field. Robust for the known Claude Code event schema.
EventSummary ParseEvent(std::string_view line);