// Current user's HKCU Run startup entry and legacy Task Scheduler logon task cleanup.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace atrium {

struct StartupState {
    std::wstring exe;
    std::wstring taskProgram;
    std::wstring runValue;
    bool hasRunValue = false;
    bool runReadOk = true;
};

struct StartupPlan {
    bool ok = false;
    bool removeTask = false;
    bool writeRun = false;
    bool deleteRun = false;
};

// The program the legacy Task Scheduler task starts, empty when there is no task.
std::wstring LogonTaskProgram();
bool RemoveLogonTask();
// The current user's Windows logon entry, pointing at this core executable.
bool SetStartOnLogin(bool enabled);
// The Run entry turned off in Task Manager's startup apps.
bool StartupTurnedOff();
bool IsStartupApprovedDisabled(const uint8_t* data, size_t size);
bool ShouldSyncStartupOnLaunch(bool development, bool registeredNow, bool startOnLogin, bool startupTurnedOff);
StartupPlan PlanStartupChange(const StartupState& state, bool enabled);

} // namespace atrium
