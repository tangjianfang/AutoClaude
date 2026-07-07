#pragma once
#include "../app.h"

// Enable the SE_SHUTDOWN_NAME privilege in this process's token.
bool EnableShutdownPrivilege();

// Execute the selected power action. In dry-run mode, performs nothing —
// returns a human-readable description so the UI can show what would have run.
const wchar_t* PerformPowerAction(Action a, bool dryRun);

const wchar_t* ActionNameW(Action a);