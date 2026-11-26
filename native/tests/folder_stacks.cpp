#include "shell/folder_stacks.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace atrium;

namespace {

bool Require(bool condition, const char* message)
{
    if (condition)
        return true;
    std::cerr << message << '\n';
    return false;
}

bool CheckNormalize()
{
    return Require(NormalizeFolderPath(L"").empty(), "empty path stays empty") &&
           Require(NormalizeFolderPath(L"   \t\r\n  ").empty(), "whitespace-only path becomes empty") &&
           Require(NormalizeFolderPath(L"  C:\\Users\\Alice\\  ") == L"C:\\Users\\Alice", "trims whitespace and trailing backslash") &&
           Require(NormalizeFolderPath(L"C:\\Users\\Alice\\\\\\") == L"C:\\Users\\Alice", "strips multiple trailing backslashes") &&
           Require(NormalizeFolderPath(L"D:/Work/Projects/") == L"D:/Work/Projects", "strips trailing forward slash") &&
           Require(NormalizeFolderPath(L"C:\\") == L"C:\\", "preserves drive root backslash") &&
           Require(NormalizeFolderPath(L"C:\\\\\\") == L"C:\\", "collapses extra slashes on drive root") &&
           Require(NormalizeFolderPath(L"C:/") == L"C:/", "preserves drive root forward slash") &&
           Require(NormalizeFolderPath(L"\\\\?\\C:\\") == L"\\\\?\\C:\\", "preserves extended drive root") &&
           Require(NormalizeFolderPath(L"\\") == L"\\", "preserves single root slash") &&
           Require(NormalizeFolderPath(L"  \\\\server\\share\\  ") == L"\\\\server\\share", "strips trailing slash from UNC share");
}

bool CheckDedupe()
{
    if (!Require(DedupeFolders({}, 8).empty(), "empty input yields empty output"))
        return false;
    if (!Require(DedupeFolders({L"C:\\Temp"}, 0).empty(), "maxCount 0 yields empty output"))
        return false;

    const std::vector<std::wstring> raw = {
        L"   ", L"  C:\\Users\\Docs\\ ", L"c:\\users\\docs", L"C:/Users/Docs/", L"D:\\Projects\\", L"C:\\Übung", L"c:\\übung\\", L"E:\\Extra",
    };
    const std::vector<std::wstring> deduped = DedupeFolders(raw, 8);
    if (!Require(deduped.size() == 4, "dedupe keeps 4 unique folders"))
        return false;
    if (!Require(deduped[0] == L"C:\\Users\\Docs" && deduped[1] == L"D:\\Projects" && deduped[2] == L"C:\\Übung" && deduped[3] == L"E:\\Extra",
                 "dedupe preserves first normalized occurrence"))
        return false;

    const std::vector<std::wstring> capped = DedupeFolders(raw, 2);
    return Require(capped.size() == 2 && capped[0] == L"C:\\Users\\Docs" && capped[1] == L"D:\\Projects", "dedupe truncates to maxCount");
}

bool CheckModelRemoteAndMissing()
{
    FolderStackModel model(nullptr, 0);
    model.SetFolders({L"\\\\nonexistent-server\\share\\docs\\", L"\\\\NONEXISTENT-SERVER\\SHARE\\DOCS", L"C:\\__atrium_missing_folder_test__\\"},
                     {L"\\\\nonexistent-server\\share\\deck.pptx", L"C:\\__atrium_missing_file_test__.pdf"}, 32);

    std::vector<StackFolder> snap;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        snap = model.Snapshot();
        if (!snap.empty())
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    return Require(snap.size() == 4, "model resolves folders and files") &&
           Require(snap[0].path == L"\\\\nonexistent-server\\share\\docs" && snap[0].name == L"docs" && !snap[0].missing && !snap[0].isFile &&
                       snap[0].icon.Empty(),
                   "remote path skips disk probe and uses tail segment") &&
           Require(snap[1].path == L"C:\\__atrium_missing_folder_test__" && snap[1].name == L"__atrium_missing_folder_test__" && snap[1].missing &&
                       !snap[1].isFile,
                   "missing local path marks missing and keeps tail name") &&
           Require(snap[2].path == L"\\\\nonexistent-server\\share\\deck.pptx" && snap[2].name == L"deck" && !snap[2].missing && snap[2].isFile &&
                       snap[2].icon.Empty(),
                   "remote file strips extension and sets isFile") &&
           Require(snap[3].path == L"C:\\__atrium_missing_file_test__.pdf" && snap[3].name == L"__atrium_missing_file_test__" && snap[3].missing &&
                       snap[3].isFile,
                   "missing local file marks missing and strips extension");
}

} // namespace

int main()
{
    if (!CheckNormalize() || !CheckDedupe() || !CheckModelRemoteAndMissing())
        return 1;
    std::cout << "folder stacks: all checks passed\n";
    return 0;
}
