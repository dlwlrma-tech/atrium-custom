#include "shell/netease_tracks.h"

#include "log.h"

#include "win32_path.h"

#include <windows.h>
#include <roapi.h>
#include <shlobj.h>
#include <windows.data.json.h>
#include <winstring.h>
#include <winsqlite/winsqlite3.h>

#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>
#include <vector>

namespace atrium {

namespace {

namespace Wdj = ABI::Windows::Data::Json;
using JsonVector = __FIVector_1_Windows__CData__CJson__CIJsonValue;

std::wstring TakeHString(HSTRING hs)
{
    if (!hs)
        return {};
    UINT32 len = 0;
    const wchar_t* raw = WindowsGetStringRawBuffer(hs, &len);
    std::wstring out = raw ? std::wstring(raw, len) : std::wstring{};
    WindowsDeleteString(hs);
    return out;
}

bool TryParseJsonObject(const std::wstring& text, winrt::com_ptr<Wdj::IJsonObject>& out)
{
    out = nullptr;
    if (text.empty())
        return false;
    HSTRING_HEADER clsHdr{};
    HSTRING cls = nullptr;
    const wchar_t* name = RuntimeClass_Windows_Data_Json_JsonObject;
    if (FAILED(WindowsCreateStringReference(name, static_cast<UINT32>(wcslen(name)), &clsHdr, &cls)))
        return false;
    winrt::com_ptr<Wdj::IJsonObjectStatics> statics;
    if (FAILED(RoGetActivationFactory(cls, IID_PPV_ARGS(statics.put()))) || !statics)
        return false;
    HSTRING_HEADER inHdr{};
    HSTRING inHs = nullptr;
    if (FAILED(WindowsCreateStringReference(text.c_str(), static_cast<UINT32>(text.size()), &inHdr, &inHs)))
        return false;
    boolean ok = FALSE;
    if (FAILED(statics->TryParse(inHs, out.put(), &ok)) || !ok || !out) {
        out = nullptr;
        return false;
    }
    return true;
}

bool NamedValue(Wdj::IJsonObject* obj, const wchar_t* key, winrt::com_ptr<Wdj::IJsonValue>& out)
{
    out = nullptr;
    if (!obj || !key)
        return false;
    HSTRING_HEADER hdr{};
    HSTRING hs = nullptr;
    if (FAILED(WindowsCreateStringReference(key, static_cast<UINT32>(wcslen(key)), &hdr, &hs)))
        return false;
    if (FAILED(obj->GetNamedValue(hs, out.put())) || !out) {
        out = nullptr;
        return false;
    }
    return true;
}

Wdj::JsonValueType JsonType(Wdj::IJsonValue* v)
{
    Wdj::JsonValueType t = Wdj::JsonValueType_Null;
    if (!v || FAILED(v->get_ValueType(&t)))
        return Wdj::JsonValueType_Null;
    return t;
}

bool JsonString(Wdj::IJsonValue* v, std::wstring& out)
{
    if (JsonType(v) != Wdj::JsonValueType_String)
        return false;
    HSTRING hs = nullptr;
    if (FAILED(v->GetString(&hs)))
        return false;
    out = TakeHString(hs);
    return true;
}

bool JsonNumber(Wdj::IJsonValue* v, double& out) { return JsonType(v) == Wdj::JsonValueType_Number && SUCCEEDED(v->GetNumber(&out)); }

bool JsonObjectVal(Wdj::IJsonValue* v, winrt::com_ptr<Wdj::IJsonObject>& out)
{
    out = nullptr;
    return JsonType(v) == Wdj::JsonValueType_Object && SUCCEEDED(v->GetObject(out.put())) && out != nullptr;
}

bool JsonArrayVal(Wdj::IJsonValue* v, winrt::com_ptr<JsonVector>& out)
{
    out = nullptr;
    winrt::com_ptr<Wdj::IJsonArray> arr;
    if (JsonType(v) != Wdj::JsonValueType_Array || FAILED(v->GetArray(arr.put())) || !arr)
        return false;
    out = arr.try_as<JsonVector>();
    return out != nullptr;
}

const std::wstring& PlayerRoot()
{
    static const std::wstring kRoot = [] {
        PWSTR dir = nullptr;
        std::wstring out;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &dir)) && dir)
            out = std::wstring(dir) + L"\\NetEase\\CloudMusic\\";
        CoTaskMemFree(dir);
        return out;
    }();
    return kRoot;
}

std::wstring PlayerPath(const wchar_t* within)
{
    const std::wstring& root = PlayerRoot();
    return root.empty() ? std::wstring() : root + within;
}

std::wstring QueuePath() { return PlayerPath(L"webdata\\file\\playingList"); }

std::wstring ParseJsonId(Wdj::IJsonValue* v)
{
    std::wstring s;
    if (JsonString(v, s))
        return s;
    double n = 0.0;
    if (JsonNumber(v, n) && std::isfinite(n))
        return std::to_wstring(static_cast<int64_t>(n));
    return {};
}

uint64_t WriteTime(const std::wstring& path, uint64_t* size = nullptr)
{
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
        return 0;
    if (size)
        *size = (uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    return (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
}

std::wstring Utf8(const unsigned char* s, int bytes)
{
    const int n = s ? MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(s), bytes, nullptr, 0) : 0;
    std::wstring out(size_t(std::max(n, 0)), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(s), bytes, out.data(), n);
    return out;
}

bool IsBusyError(int rc) { return rc == SQLITE_BUSY || rc == SQLITE_LOCKED; }

// The first column of each row `sql` gives (one text parameter bound), until `row` returns false.
template<class Row> bool Query(sqlite3* db, const char* sql, const std::string& param, Row row, bool* busy = nullptr)
{
    sqlite3_stmt* st = nullptr;
    const int prep = sqlite3_prepare_v2(db, sql, -1, &st, nullptr);
    if (prep != SQLITE_OK) {
        if (busy && IsBusyError(prep))
            *busy = true;
        return false;
    }
    if (!param.empty())
        sqlite3_bind_text(st, 1, param.c_str(), int(param.size()), SQLITE_TRANSIENT);
    int rc = SQLITE_ROW;
    while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
        if (!row(Utf8(sqlite3_column_text(st, 0), sqlite3_column_bytes(st, 0))))
            break;
    }
    sqlite3_finalize(st);
    if (busy && IsBusyError(rc))
        *busy = true;
    return rc == SQLITE_ROW || rc == SQLITE_DONE;
}

// The liked playlist's id: the player's record of it, else in its cached account playlists the playlist object holding
// specialType 5 also holds its comment thread, "A_PL_0_<id>" (the cache stores the JSON escaped).
std::wstring LikedPlaylist(sqlite3* db, bool& retryable)
{
    std::wstring found;
    // The player's own record of it, newest first: "starPlaylistId":"<id>".
    Query(
        db, "SELECT jsonStr FROM persistentModel WHERE jsonStr LIKE '%starPlaylistId%' ORDER BY time DESC", {},
        [&](const std::wstring& j) {
            const size_t at = j.find(L"starPlaylistId");
            size_t v = at == std::wstring::npos ? j.size() : at + 14;
            while (v < j.size() && (j[v] == L'\\' || j[v] == L'"' || j[v] == L':' || j[v] == L' '))
                ++v;
            size_t end = v;
            while (end < j.size() && iswdigit(j[end]))
                ++end;
            found = j.substr(v, end - v);
            return found.empty();
        },
        &retryable);
    if (!found.empty() || retryable)
        return found;
    Query(
        db, "SELECT jsonStr FROM requestCache WHERE id LIKE '%/user/playlist%' AND jsonStr LIKE '%specialType%'", {},
        [&](const std::wstring& j) {
            for (size_t at = j.find(L"specialType"); at != std::wstring::npos && found.empty(); at = j.find(L"specialType", at + 1)) {
                size_t v = at + 11;
                while (v < j.size() && (j[v] == L'\\' || j[v] == L'"' || j[v] == L':' || j[v] == L' '))
                    ++v;
                if (v >= j.size() || j[v] != L'5' || (v + 1 < j.size() && iswdigit(j[v + 1])))
                    continue;
                const size_t from = at > 1500 ? at - 1500 : 0, thread = j.rfind(L"A_PL_0_", at);
                if (thread == std::wstring::npos || thread < from)
                    continue;
                size_t end = thread + 7;
                while (end < j.size() && iswdigit(j[end]))
                    ++end;
                found = j.substr(thread + 7, end - thread - 7);
            }
            return found.empty();
        },
        &retryable);
    return found;
}

std::string Narrow(const std::wstring& s)
{
    std::string out;
    for (wchar_t c : s)
        out += char(c); // digits only
    return out;
}

// The file as text (UTF-8), up to 32 MB; empty if unreadable.
std::wstring ReadText(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    LARGE_INTEGER size{};
    std::string bytes;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (32LL << 20)) {
        bytes.resize(size_t(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(file, bytes.data(), DWORD(bytes.size()), &read, nullptr))
            read = 0;
        bytes.resize(read);
    }
    CloseHandle(file);
    const int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), nullptr, 0);
    std::wstring text(size_t(std::max(n, 0)), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, bytes.data(), int(bytes.size()), text.data(), n);
    return text;
}

NeteaseQueuedTrack ParseTrackObject(Wdj::IJsonObject* track)
{
    NeteaseQueuedTrack t;
    if (!track)
        return t;
    winrt::com_ptr<Wdj::IJsonValue> val;
    if (NamedValue(track, L"name", val))
        JsonString(val.get(), t.title);
    double ms = 0.0;
    if (NamedValue(track, L"duration", val) && JsonNumber(val.get(), ms) && std::isfinite(ms) && ms > 0.0 && ms < 86400000.0)
        t.lengthUs = static_cast<int64_t>(std::llround(ms * 1000.0));
    winrt::com_ptr<JsonVector> artists;
    if (NamedValue(track, L"artists", val) && JsonArrayVal(val.get(), artists)) {
        UINT32 count = 0;
        if (SUCCEEDED(artists->get_Size(&count))) {
            for (UINT32 i = 0; i < count; ++i) {
                winrt::com_ptr<Wdj::IJsonValue> elem;
                winrt::com_ptr<Wdj::IJsonObject> obj;
                std::wstring artistName;
                if (SUCCEEDED(artists->GetAt(i, elem.put())) && JsonObjectVal(elem.get(), obj) && NamedValue(obj.get(), L"name", val) &&
                    JsonString(val.get(), artistName)) {
                    t.artists += (t.artists.empty() ? L"" : L"/") + artistName;
                }
            }
        }
    }
    return t;
}

bool ParseQueue(const std::wstring& text, std::vector<NeteaseQueuedTrack>& out)
{
    out.clear();
    winrt::com_ptr<Wdj::IJsonObject> root;
    winrt::com_ptr<Wdj::IJsonValue> listVal;
    winrt::com_ptr<JsonVector> list;
    if (!TryParseJsonObject(text, root) || !NamedValue(root.get(), L"list", listVal) || !JsonArrayVal(listVal.get(), list))
        return false;
    UINT32 count = 0;
    if (FAILED(list->get_Size(&count)))
        return false;
    for (UINT32 i = 0; i < count; ++i) {
        winrt::com_ptr<Wdj::IJsonValue> item;
        winrt::com_ptr<Wdj::IJsonObject> entry;
        winrt::com_ptr<Wdj::IJsonValue> trackVal;
        winrt::com_ptr<Wdj::IJsonObject> trackObj;
        if (FAILED(list->GetAt(i, item.put())) || !JsonObjectVal(item.get(), entry) || !NamedValue(entry.get(), L"track", trackVal) ||
            !JsonObjectVal(trackVal.get(), trackObj))
            continue;
        NeteaseQueuedTrack t = ParseTrackObject(trackObj.get());
        winrt::com_ptr<Wdj::IJsonValue> idVal;
        if (NamedValue(entry.get(), L"id", idVal))
            t.id = ParseJsonId(idVal.get());
        if (!t.title.empty() && t.lengthUs > 0)
            out.push_back(std::move(t));
    }
    return true;
}

} // namespace

bool NeteaseQueueCache::Update(const std::wstring& text, uint64_t writeTime, uint64_t sizeBytes)
{
    if (text.empty())
        return false;
    std::vector<NeteaseQueuedTrack> parsed;
    if (!ParseQueue(text, parsed))
        return false;
    tracks_ = std::move(parsed);
    writeTime_ = writeTime;
    sizeBytes_ = sizeBytes;
    return true;
}

const NeteaseQueuedTrack* NeteaseQueueCache::Find(const std::wstring& title, const std::wstring& artist) const
{
    const std::wstring wantArtist = FoldCase(artist);
    const NeteaseQueuedTrack* byName = nullptr;
    for (const NeteaseQueuedTrack& t : tracks_) {
        if (t.title != title)
            continue;
        const std::wstring has = FoldCase(t.artists);
        if (!wantArtist.empty() && !has.empty() &&
            (has == wantArtist || wantArtist.find(has) != std::wstring::npos || has.find(wantArtist) != std::wstring::npos))
            return &t;
        if (!byName)
            byName = &t;
    }
    return byName;
}

int64_t NeteaseQueueCache::FindLengthUs(const std::wstring& title, const std::wstring& artist) const
{
    const NeteaseQueuedTrack* t = Find(title, artist);
    return t ? t->lengthUs : 0;
}

// The queue, read again when its file changes.
const NeteaseQueueCache& Queue()
{
    static NeteaseQueueCache cache;
    const std::wstring path = QueuePath();
    uint64_t size = 0;
    const uint64_t writeTime = WriteTime(path, &size);
    if (writeTime && cache.NeedsRead(writeTime, size)) {
        const std::wstring text = ReadText(path);
        if (!text.empty())
            cache.Update(text, writeTime, size);
    }
    return cache;
}

int64_t NeteaseTrackLengthUs(const std::wstring& title, const std::wstring& artist) { return Queue().FindLengthUs(title, artist); }

std::optional<bool> NeteaseLikes::Liked(const std::wstring& title, const std::wstring& artist)
{
    const std::wstring path = PlayerPath(L"Library\\webdb.dat");
    const uint64_t writeTime = WriteTime(path);
    if (!writeTime)
        return std::nullopt;
    // Read again when the player wrote, and a busy/open-failed read again a moment later.
    const ULONGLONG now = GetTickCount64();
    if (writeTime != writeTime_ || (retryable_ && now - readAt_ > 3000)) {
        writeTime_ = writeTime;
        readAt_ = now;
        retryable_ = false;
        ok_ = Read(path, retryable_);
    }
    const std::wstring id = CurrentId(title, artist);
    std::optional<bool> liked;
    if (ok_ && !id.empty())
        liked = ids_.contains(id);
    const std::wstring said = title + L'\n' + std::to_wstring(liked ? int(*liked) : -1);
    if (said != logged_) {
        logged_ = said;
        Log(L"netease: heart of '%s': %d (read %d, %zu liked, track %s)", title.c_str(), liked ? int(*liked) : -1, ok_, ids_.size(), id.c_str());
    }
    return liked;
}

// The track's id: the one the player last began when it is this one (its history, by name), else the
// queue's by title and artist.
std::wstring NeteaseLikes::CurrentId(const std::wstring& title, const std::wstring& artist) const
{
    const std::wstring a = FoldCase(title), b = FoldCase(recentName_);
    if (!recentId_.empty() && !a.empty() && a == b)
        return recentId_;
    const NeteaseQueuedTrack* track = Queue().Find(title, artist);
    return track ? track->id : std::wstring();
}

bool NeteaseLikes::Read(const std::wstring& path, bool& retryable)
{
    retryable = false;
    sqlite3* db = nullptr;
    std::string utf8(size_t(WideCharToMultiByte(CP_UTF8, 0, path.c_str(), int(path.size()), nullptr, 0, nullptr, nullptr)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), int(path.size()), utf8.data(), int(utf8.size()), nullptr, nullptr);
    if (sqlite3_open_v2(utf8.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        retryable = true;
        return false;
    }
    sqlite3_busy_timeout(db, 200); // the player may be writing
    if (playlist_.empty())
        playlist_ = LikedPlaylist(db, retryable);
    Query(
        db, "SELECT jsonStr FROM historyTracks ORDER BY playtime DESC LIMIT 1", {},
        [&](const std::wstring& text) {
            winrt::com_ptr<Wdj::IJsonObject> o;
            winrt::com_ptr<Wdj::IJsonValue> idVal, nameVal;
            std::wstring name;
            if (TryParseJsonObject(text, o) && NamedValue(o.get(), L"id", idVal) && NamedValue(o.get(), L"name", nameVal) && JsonString(nameVal.get(), name)) {
                recentId_ = ParseJsonId(idVal.get());
                recentName_ = recentId_.empty() ? std::wstring() : std::move(name);
            }
            return false;
        },
        &retryable);
    bool found = false;
    std::unordered_set<std::wstring> ids;
    if (!playlist_.empty() && !retryable) {
        Query(
            db, "SELECT jsonStr FROM playlistTrackIds WHERE id = ?", Narrow(playlist_),
            [&](const std::wstring& text) {
                winrt::com_ptr<Wdj::IJsonObject> o;
                winrt::com_ptr<Wdj::IJsonValue> trackIdsVal;
                winrt::com_ptr<JsonVector> trackIds;
                if (!TryParseJsonObject(text, o) || !NamedValue(o.get(), L"trackIds", trackIdsVal) || !JsonArrayVal(trackIdsVal.get(), trackIds))
                    return false;
                UINT32 count = 0;
                if (FAILED(trackIds->get_Size(&count)))
                    return false;
                for (UINT32 i = 0; i < count; ++i) {
                    winrt::com_ptr<Wdj::IJsonValue> v, idVal;
                    winrt::com_ptr<Wdj::IJsonObject> entry;
                    if (FAILED(trackIds->GetAt(i, v.put())) || !v)
                        continue;
                    std::wstring tid;
                    if (JsonObjectVal(v.get(), entry) && NamedValue(entry.get(), L"id", idVal))
                        tid = ParseJsonId(idVal.get());
                    else
                        tid = ParseJsonId(v.get());
                    if (!tid.empty())
                        ids.insert(std::move(tid));
                }
                found = true;
                return false;
            },
            &retryable);
    }
    sqlite3_close(db);
    if (retryable)
        return false;
    if (!found) {
        playlist_.clear(); // looked for again when DB changes
        ids_.clear();
        return false;
    }
    ids_ = std::move(ids);
    return true;
}

void NeteaseTimeline::Fill(const std::wstring& title, const std::wstring& artist, bool playing, double nowMs, MediaTimeline& t)
{
    const std::wstring track = title + L'\n' + artist;
    const bool changed = track != track_ || playing != playing_;
    // The queue may be written a moment after the track starts: asked again while it has no length.
    if (track != track_ || (lengthUs_ == 0 && nowMs - lookedUpMs_ > 2000)) {
        lengthUs_ = NeteaseTrackLengthUs(title, artist);
        lookedUpMs_ = nowMs;
    }
    track_ = track;
    playing_ = playing;
    MediaTimeline est = t;
    estimate_.Update(track, playing, lengthUs_, nowMs, est);
    if (changed || nowMs - readMs_ >= 500) {
        readMs_ = nowMs;
        double pos = 0, len = 0;
        const bool wasFromMemory = fromMemory_;
        fromMemory_ = memory_.Read(double(lengthUs_) / 1e6, playing, nowMs, pos, len);
        if (fromMemory_) {
            MediaTimeline m;
            m.endUs = int64_t(std::llround(len * 1e6));
            m.anchorPosUs = int64_t(std::llround(pos * 1e6));
            m.anchorMs = nowMs;
            m.playing = playing;
            if (!wasFromMemory || changed || last_.playing != m.playing || last_.endUs != m.endUs ||
                std::llabs(last_.PositionUs(nowMs) - m.anchorPosUs) > 250'000)
                last_ = m;
        }
    }
    if (fromMemory_)
        t = last_;
    else
        t = est;
}

} // namespace atrium
