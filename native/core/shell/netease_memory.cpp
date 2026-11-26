#include "shell/netease_memory.h"

#include "log.h"

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_set>

namespace atrium {

namespace {

// Versions whose places are known (cloudmusic.dll's PE time stamp and image size), oldest first. The
// first is also where a new version's search starts.
struct KnownVersion {
    uint32_t stamp, size;
    uintptr_t position, length;
};
constexpr KnownVersion kKnown[] = {
    {0x6ad712e1, 41054208, 0x21a74d8, 0x2214778},
    {0x6ae94a6a, 41058304, 0x21a84d8, 0x2215798},
};
constexpr uintptr_t kKnownPosition = kKnown[0].position, kKnownLength = kKnown[0].length;
constexpr double kRetryMs = 5000;      // nothing found: try again after
constexpr double kProbeMs = 900;       // between two readings that must differ by the time between them
constexpr double kTolerance = 0.3;     // s
constexpr double kMismatchResetMs = 5000;
constexpr size_t kChunk = 1 << 20;     // bytes read at a time
constexpr size_t kFewCandidates = 64;  // fewer: read one by one rather than scanned again
constexpr int kMaxFailures = 4;        // full scans in vain: the process is left alone
constexpr DWORD kReadableProtectMask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

struct ProcEntry {
    DWORD pid = 0;
    DWORD ppid = 0;
};

bool ValidLength(double v) { return std::isfinite(v) && v > 0 && v < 86400; }

bool InsideSections(const std::vector<std::pair<uintptr_t, uintptr_t>>& sections, uintptr_t rva)
{
    return std::ranges::any_of(sections,
                               [&](const auto& s) { return rva >= s.first && s.second >= sizeof(double) && rva - s.first <= s.second - sizeof(double); });
}

} // namespace

bool NeteaseMemory::LoadPeSections(HMODULE module, DWORD pid)
{
    base_ = reinterpret_cast<uintptr_t>(module);
    // The PE header: the version, and the writable sections where the values live.
    std::array<uint8_t, 4096> header{};
    SIZE_T read = 0;
    if (!ReadProcessMemory(process_.get(), module, header.data(), header.size(), &read) || read < sizeof(IMAGE_DOS_HEADER))
        return false;

    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, header.data(), sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < static_cast<LONG>(sizeof(IMAGE_DOS_HEADER)))
        return false;
    const size_t peAt = static_cast<size_t>(dos.e_lfanew);
    if (peAt > read || read - peAt < 4 + sizeof(IMAGE_FILE_HEADER))
        return false;

    uint32_t ntSig = 0;
    std::memcpy(&ntSig, header.data() + peAt, sizeof(ntSig));
    if (ntSig != IMAGE_NT_SIGNATURE)
        return false;

    IMAGE_FILE_HEADER fileHdr{};
    std::memcpy(&fileHdr, header.data() + peAt + 4, sizeof(fileHdr));
    if (fileHdr.SizeOfOptionalHeader < 60)
        return false;
    const size_t optAt = peAt + 4 + sizeof(IMAGE_FILE_HEADER);
    if (optAt > read || fileHdr.SizeOfOptionalHeader > read - optAt)
        return false;

    stamp_ = fileHdr.TimeDateStamp;
    std::memcpy(&imageSize_, header.data() + optAt + 56, sizeof(imageSize_));
    if (imageSize_ == 0)
        return false;

    const size_t secTableAt = optAt + fileHdr.SizeOfOptionalHeader;
    for (size_t s = 0; s < fileHdr.NumberOfSections; ++s) {
        const size_t at = secTableAt + s * sizeof(IMAGE_SECTION_HEADER);
        if (at > read || read - at < sizeof(IMAGE_SECTION_HEADER))
            break;
        IMAGE_SECTION_HEADER sec{};
        std::memcpy(&sec, header.data() + at, sizeof(sec));
        if ((sec.Characteristics & IMAGE_SCN_MEM_WRITE) == 0 || sec.VirtualAddress >= imageSize_)
            continue;
        const uint32_t avail = imageSize_ - sec.VirtualAddress;
        const uintptr_t clampedSize = std::min<uint32_t>(sec.Misc.VirtualSize, avail);
        if (clampedSize >= sizeof(double))
            sections_.emplace_back(sec.VirtualAddress, clampedSize);
    }
    if (sections_.empty())
        return false;
    Log(L"netease: cloudmusic.dll %08x/%u in %lu", stamp_, imageSize_, pid);
    return true;
}

bool NeteaseMemory::TryAttachProcess(DWORD pid)
{
    winrt::handle process{OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid)};
    if (!process)
        return false;
    std::array<HMODULE, 1024> modules{};
    DWORD needed = 0;
    if (!K32EnumProcessModules(process.get(), modules.data(), DWORD(sizeof(modules)), &needed))
        return false;
    const size_t modCount = std::min<size_t>(needed / sizeof(HMODULE), modules.size());
    for (size_t i = 0; i < modCount; ++i) {
        wchar_t name[MAX_PATH] = {};
        if (!K32GetModuleBaseNameW(process.get(), modules[i], name, MAX_PATH) || _wcsicmp(name, L"cloudmusic.dll") != 0)
            continue;
        process_ = std::move(process);
        attachedPid_ = pid;
        if (LoadPeSections(modules[i], pid))
            return true;
        Detach();
        return false;
    }
    return false;
}

bool NeteaseMemory::Attach()
{
    const DWORD preferredAvoid = avoidPid_;
    avoidPid_ = 0;
    Detach();
    winrt::handle snap{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    if (!snap)
        return false;
    std::vector<ProcEntry> procs;
    std::unordered_set<DWORD> pids;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap.get(), &pe); ok; ok = Process32NextW(snap.get(), &pe)) {
        if (_wcsicmp(pe.szExeFile, L"cloudmusic.exe") != 0)
            continue;
        procs.push_back({pe.th32ProcessID, pe.th32ParentProcessID});
        pids.insert(pe.th32ProcessID);
    }

    // Prefer root browser processes: CEF child processes also map cloudmusic.dll without holding playback state.
    for (int pass = 0; pass < (preferredAvoid ? 2 : 1); ++pass) {
        for (const auto& entry : procs) {
            if (pids.contains(entry.ppid))
                continue;
            const bool isAvoided = preferredAvoid != 0 && entry.pid == preferredAvoid;
            if ((pass == 0) == isAvoided)
                continue;
            if (TryAttachProcess(entry.pid))
                return true;
        }
    }
    return false;
}

void NeteaseMemory::ResetMatch()
{
    lengthRva_ = positionRva_ = 0;
    trusted_ = false;
    candidates_.clear();
    probeMs_ = 0;
    failures_ = 0;
    mismatchSinceMs_ = 0;
}

void NeteaseMemory::Detach()
{
    process_.close();
    attachedPid_ = 0;
    base_ = 0;
    stamp_ = 0;
    imageSize_ = 0;
    sections_.clear();
    ResetMatch();
}

bool NeteaseMemory::ReadDouble(uintptr_t rva, double& value) const
{
    SIZE_T read = 0;
    return ReadProcessMemory(process_.get(), reinterpret_cast<LPCVOID>(base_ + rva), &value, sizeof(value), &read) && read == sizeof(value);
}

void NeteaseMemory::ScanRegion(uintptr_t rva, uintptr_t avail, double lo, double hi, std::vector<uint8_t>& bytes, Doubles& found) const
{
    for (uintptr_t off = 0; off + sizeof(double) <= avail; off += kChunk) {
        const size_t n = std::min<size_t>(kChunk, static_cast<size_t>(avail - off)) & ~size_t(7);
        if (n < sizeof(double))
            break;
        SIZE_T read = 0;
        if (!ReadProcessMemory(process_.get(), reinterpret_cast<LPCVOID>(base_ + rva + off), bytes.data(), n, &read))
            continue;
        for (size_t i = 0; i + 8 <= read; i += 8) {
            double v = 0;
            std::memcpy(&v, bytes.data() + i, 8);
            if (std::isfinite(v) && v >= lo && v <= hi)
                found.emplace_back(rva + off + i, v);
        }
    }
}

NeteaseMemory::Doubles NeteaseMemory::Scan(double lo, double hi) const
{
    Doubles found;
    std::vector<uint8_t> bytes(kChunk);
    for (const auto& [rva, size] : sections_) {
        uintptr_t curRva = rva;
        const uintptr_t endRva = rva + size;
        while (curRva + sizeof(double) <= endRva) {
            MEMORY_BASIC_INFORMATION mbi{};
            const uintptr_t curAddr = base_ + curRva;
            if (VirtualQueryEx(process_.get(), reinterpret_cast<LPCVOID>(curAddr), &mbi, sizeof(mbi)) == 0)
                break;
            const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (regionEnd <= curAddr)
                break;
            const uintptr_t avail = std::min<uintptr_t>(endRva - curRva, regionEnd - curAddr);
            const bool readable = mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 && (mbi.Protect & kReadableProtectMask) != 0;
            if (readable && avail >= sizeof(double))
                ScanRegion(curRva, avail, lo, hi, bytes, found);
            curRva += avail;
        }
    }
    return found;
}

// The length's place: a known version's, else the one double equal to the queue's length (nearest
// the known place if several); the position's first guess moved with it.
bool NeteaseMemory::Locate(double lengthSec)
{
    for (const KnownVersion& v : kKnown)
        if (stamp_ == v.stamp && imageSize_ == v.size && InsideSections(sections_, v.length) && InsideSections(sections_, v.position)) {
            lengthRva_ = v.length;
            positionRva_ = v.position;
            trusted_ = true;
            return true;
        }
    if (!ValidLength(lengthSec))
        return false;
    const Doubles equal = Scan(lengthSec - 0.0005, lengthSec + 0.0005);
    if (equal.empty())
        return false;
    auto distance = [](uintptr_t rva) { return rva > kKnownLength ? rva - kKnownLength : kKnownLength - rva; };
    lengthRva_ = std::ranges::min(equal, {}, [&](const auto& e) { return distance(e.first); }).first;
    candidates_.clear();
    probeMs_ = 0;
    constexpr uintptr_t kDelta = kKnownLength - kKnownPosition;
    if (lengthRva_ >= kDelta) {
        const uintptr_t guess = lengthRva_ - kDelta;
        double v = 0;
        if (InsideSections(sections_, guess) && ReadDouble(guess, v))
            candidates_.emplace_back(guess, v);
        Log(L"netease: the length at %llx (%zu equal), the position first tried at %llx", uint64_t(lengthRva_), equal.size(), uint64_t(guess));
    } else {
        Log(L"netease: the length at %llx (%zu equal)", uint64_t(lengthRva_), equal.size());
    }
    return true;
}

// The position's place: of the candidates, the one that advanced by the time between two readings
// while playing; with no candidate left, every double up to the length is one.
void NeteaseMemory::Verify(bool playing, double nowMs, double length)
{
    if (!playing) {
        probeMs_ = 0; // paused: start over from the next reading while playing
        return;
    }
    if (probeMs_ == 0) {
        if (candidates_.empty())
            candidates_ = Scan(0, length);
        else
            for (auto& [rva, value] : candidates_)
                ReadDouble(rva, value);
        probeMs_ = nowMs;
        return;
    }
    if (nowMs - probeMs_ < kProbeMs)
        return;
    const double elapsed = (nowMs - probeMs_) / 1000;
    Doubles moved;
    if (candidates_.size() <= kFewCandidates) {
        for (const auto& [rva, before] : candidates_)
            if (double now = 0; ReadDouble(rva, now) && std::abs(now - before - elapsed) < kTolerance)
                moved.emplace_back(rva, now);
    } else {
        const Doubles again = Scan(0, length);
        auto a = candidates_.begin();
        for (const auto& [rva, now] : again) {
            while (a != candidates_.end() && a->first < rva)
                ++a;
            if (a != candidates_.end() && a->first == rva && std::abs(now - a->second - elapsed) < kTolerance)
                moved.emplace_back(rva, now);
        }
    }
    probeMs_ = nowMs;
    if (moved.size() == 1) {
        positionRva_ = moved.front().first;
        trusted_ = true;
        candidates_.clear();
        Log(L"netease: the position at %llx", uint64_t(positionRva_));
    } else if (moved.empty()) {
        candidates_.clear(); // the guess (or every candidate) failed: a full scan next
        if (++failures_ >= kMaxFailures) {
            Log(L"netease: no position found; the track's time is estimated");
            retryMs_ = nowMs + 600000; // ten minutes: perhaps the player reloads
        }
    } else {
        candidates_ = std::move(moved); // several still: the next reading decides
    }
}

bool NeteaseMemory::Read(double lengthSec, bool playing, double nowMs, double& position, double& length)
{
    if (retryMs_ > 0) {
        if (nowMs < retryMs_)
            return false;
        retryMs_ = 0;
        failures_ = 0;
        candidates_.clear();
        probeMs_ = 0;
    }
    DWORD code = 0;
    if (process_ && (!GetExitCodeProcess(process_.get(), &code) || code != STILL_ACTIVE))
        Detach();
    if ((!process_ && !Attach()) || (!lengthRva_ && !Locate(lengthSec))) {
        retryMs_ = nowMs + kRetryMs;
        return false;
    }
    double len = 0, pos = 0;
    if (!ReadDouble(lengthRva_, len) || !ValidLength(len)) {
        const DWORD prevPid = attachedPid_;
        Detach();
        avoidPid_ = prevPid;
        retryMs_ = nowMs + kRetryMs;
        return false;
    }
    if (lengthSec > 0 && std::abs(len - lengthSec) > 0.5) {
        if (mismatchSinceMs_ == 0)
            mismatchSinceMs_ = nowMs;
        else if (!trusted_ && nowMs - mismatchSinceMs_ > kMismatchResetMs) {
            Log(L"netease: length mismatch persisted; resetting match");
            ResetMatch();
        }
        return false; // the player has not caught up with the track yet
    }
    mismatchSinceMs_ = 0;
    if (!trusted_)
        Verify(playing, nowMs, len);
    if (!trusted_ || !ReadDouble(positionRva_, pos) || !std::isfinite(pos) || pos < 0 || pos > len + 1)
        return false;
    position = std::min(pos, len);
    length = len;
    return true;
}

} // namespace atrium
