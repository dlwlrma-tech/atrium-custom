#include "shell/logon_task.h"

#include "log.h"
#include "settings.h"

#include <windows.h>
#include <sddl.h>
#include <taskschd.h>

#include <winrt/base.h>

#include <iterator>
#include <mutex>

namespace atrium {

namespace {

struct ComScope {
    bool owned;
    ComScope() : owned(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {}
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    ~ComScope()
    {
        if (owned)
            CoUninitialize();
    }
};

struct Bstr {
    BSTR b;
    explicit Bstr(const std::wstring& s) : b(SysAllocStringLen(s.data(), UINT(s.size()))) {}
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    ~Bstr() { SysFreeString(b); }
};

std::wstring UserSid()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return {};
    std::wstring result;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::string buffer(size, '\0');
    wchar_t* text = nullptr;
    if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size) &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) {
        result = text;
        LocalFree(text);
    }
    CloseHandle(token);
    return result;
}

// One task per user, as OneDrive names its own.
std::wstring TaskName(const std::wstring& sid) { return L"Atrium Startup-" + sid; }

winrt::com_ptr<ITaskFolder> RootFolder()
{
    winrt::com_ptr<ITaskService> service;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(service.put()))))
        return nullptr;
    VARIANT none{};
    if (FAILED(service->Connect(none, none, none, none)))
        return nullptr;
    winrt::com_ptr<ITaskFolder> folder;
    service->GetFolder(Bstr(L"\\").b, folder.put());
    return folder;
}

} // namespace

bool IsStartupApprovedDisabled(const uint8_t* data, size_t size)
{
    // The first byte is odd when the switch is off (2 on, 3 off).
    return data && size > 0 && (data[0] & 1) != 0;
}

bool ShouldSyncStartupOnLaunch(bool development, bool registeredNow, bool startOnLogin, bool startupTurnedOff)
{
    return !development && registeredNow && startOnLogin && !startupTurnedOff;
}

StartupPlan PlanStartupChange(const StartupState& state, bool enabled)
{
    if (state.exe.empty())
        return {};
    const std::wstring command = L"\"" + state.exe + L"\"";
    const bool ownTask = !state.taskProgram.empty() && _wcsicmp(state.taskProgram.c_str(), state.exe.c_str()) == 0;
    const bool otherTask = !state.taskProgram.empty() && !ownTask;
    const bool ownRun = state.hasRunValue && _wcsicmp(state.runValue.c_str(), command.c_str()) == 0;
    const bool otherRun = state.hasRunValue && !ownRun;
    if (!enabled) {
        // Another distribution's entries are not ours to remove.
        if (!state.runReadOk || otherRun || otherTask)
            return {};
        return {.ok = true, .removeTask = ownTask, .writeRun = false, .deleteRun = state.hasRunValue};
    }
    return {.ok = true, .removeTask = ownTask, .writeRun = !state.hasRunValue || state.runValue != command, .deleteRun = false};
}

std::wstring LogonTaskProgram()
{
    ComScope com;
    const std::wstring sid = UserSid();
    auto folder = sid.empty() ? nullptr : RootFolder();
    winrt::com_ptr<IRegisteredTask> task;
    winrt::com_ptr<ITaskDefinition> definition;
    winrt::com_ptr<IActionCollection> actions;
    winrt::com_ptr<IAction> action;
    if (!folder || FAILED(folder->GetTask(Bstr(TaskName(sid)).b, task.put())) || FAILED(task->get_Definition(definition.put())) ||
        FAILED(definition->get_Actions(actions.put())) || FAILED(actions->get_Item(1, action.put())))
        return {};
    auto exec = action.try_as<IExecAction>();
    BSTR path = nullptr;
    if (!exec || FAILED(exec->get_Path(&path)) || !path)
        return {};
    std::wstring result(path, SysStringLen(path));
    SysFreeString(path);
    if (result.size() >= 2 && result.front() == L'"' && result.back() == L'"')
        result = result.substr(1, result.size() - 2);
    return result;
}

bool RemoveLogonTask()
{
    ComScope com;
    const std::wstring sid = UserSid();
    auto folder = sid.empty() ? nullptr : RootFolder();
    if (!folder)
        return false;
    HRESULT hr = folder->DeleteTask(Bstr(TaskName(sid)).b, 0);
    return SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

bool SetStartOnLogin(bool enabled)
{
    if (IsDevelopmentBuild())
        return true;
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    constexpr auto kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    wchar_t exe[32768]{};
    DWORD length = GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
    if (!length || length == std::size(exe))
        return false;
    wchar_t current[32771]{};
    DWORD size = sizeof(current);
    LSTATUS read = RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"Atrium", RRF_RT_REG_SZ, nullptr, current, &size);
    const StartupState state{
        .exe = exe,
        .taskProgram = LogonTaskProgram(),
        .runValue = read == ERROR_SUCCESS ? current : std::wstring{},
        .hasRunValue = read == ERROR_SUCCESS,
        .runReadOk = read == ERROR_SUCCESS || read == ERROR_FILE_NOT_FOUND || read == ERROR_PATH_NOT_FOUND,
    };
    const StartupPlan plan = PlanStartupChange(state, enabled);
    if (!plan.ok)
        return false;
    if (plan.removeTask && !RemoveLogonTask())
        return false;
    if (plan.deleteRun) {
        LSTATUS result = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, L"Atrium");
        return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
    }
    if (plan.writeRun) {
        const std::wstring command = L"\"" + state.exe + L"\"";
        return RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"Atrium", REG_SZ, command.c_str(), DWORD((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    }
    return true;
}

bool StartupTurnedOff()
{
    BYTE state[12]{};
    DWORD size = sizeof(state);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run", L"Atrium", RRF_RT_REG_BINARY,
                        nullptr, state, &size) == ERROR_SUCCESS &&
           IsStartupApprovedDisabled(state, size);
}

} // namespace atrium
