// Long-path Win32 module/process image queries and Unicode path case folding.
#pragma once

#include <windows.h>
#include <appmodel.h>

#include <string>

namespace atrium {

inline std::wstring ModulePath(HMODULE module = nullptr)
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD len = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
        if (len == 0)
            return {};
        if (len < path.size()) {
            path.resize(len);
            return path;
        }
        if (path.size() >= 32768)
            return {};
        const size_t next = path.size() * 2;
        path.resize(next > 32768 ? 32768 : next);
    }
}

inline std::wstring ProcessImagePath(HANDLE process)
{
    if (!process)
        return {};
    wchar_t stack[MAX_PATH * 2];
    DWORD len = DWORD(sizeof(stack) / sizeof(stack[0]));
    if (QueryFullProcessImageNameW(process, 0, stack, &len))
        return std::wstring(stack, len);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return {};
    std::wstring path(32768, L'\0');
    len = DWORD(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &len))
        return {};
    path.resize(len);
    return path;
}

inline std::wstring ProcessImagePath(DWORD pid)
{
    struct ScopedHandle {
        HANDLE handle;
        explicit ScopedHandle(HANDLE h) : handle(h) {}
        ScopedHandle(const ScopedHandle&) = delete;
        ScopedHandle& operator=(const ScopedHandle&) = delete;
        ~ScopedHandle()
        {
            if (handle)
                CloseHandle(handle);
        }
    };
    const ScopedHandle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    return ProcessImagePath(process.handle);
}

inline std::wstring FoldCase(const std::wstring& text)
{
    if (text.empty())
        return text;
    const int count = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr, 0);
    if (count <= 0)
        return text;
    std::wstring folded(size_t(count), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, text.data(), int(text.size()), folded.data(), count, nullptr, nullptr, 0) <= 0)
        return text;
    return folded;
}

inline std::wstring ExeStemFolded(const std::wstring& path)
{
    std::wstring folded = FoldCase(path);
    const size_t slash = folded.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        folded = folded.substr(slash + 1);
    if (folded.size() > 4 && folded.ends_with(L".exe"))
        folded.resize(folded.size() - 4);
    return folded;
}

inline std::wstring ProcessAumid(HANDLE process)
{
    if (!process)
        return {};
    UINT32 len = 0;
    if (GetApplicationUserModelId(process, &len, nullptr) != ERROR_INSUFFICIENT_BUFFER || len == 0)
        return {};
    std::wstring aumid(len, L'\0');
    if (GetApplicationUserModelId(process, &len, aumid.data()) != ERROR_SUCCESS)
        return {};
    while (!aumid.empty() && aumid.back() == L'\0')
        aumid.pop_back();
    return aumid;
}

inline bool SameExecutableWindow(HWND window, const std::wstring& exePath = ModulePath())
{
    if (!window || exePath.empty())
        return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid)
        return false;
    const std::wstring other = ProcessImagePath(pid);
    return !other.empty() && FoldCase(other) == FoldCase(exePath);
}

inline HWND FindWindowForExecutable(const wchar_t* className, const std::wstring& exePath = ModulePath())
{
    if (!className || exePath.empty())
        return nullptr;
    for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, className, nullptr)) != nullptr;) {
        if (SameExecutableWindow(w, exePath))
            return w;
    }
    return nullptr;
}

} // namespace atrium
