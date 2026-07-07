#pragma once
#include "../app.h"
#include "../config.h"
#include "../monitor/transcript_watcher.h"
#include "../core/state_machine.h"
#include <string>

struct Layout;

void RunApp(const Config& initialCfg);