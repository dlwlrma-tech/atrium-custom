#include "shell/start_layout.h"

#include "log.h"
#include "settings.h"

#include <windows.h>

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <algorithm>
#include <cwctype>

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace {

std::wstring LayoutPath()
{
    std::wstring path = SettingsPath(); // ...\Atrium\settings.json
    return path.substr(0, path.find_last_of(L'\\') + 1) + L"start.json";
}

std::wstring Str(const json::JsonObject& o, const wchar_t* key)
{
    return o.HasKey(key) && o.GetNamedValue(key).ValueType() == json::JsonValueType::String ? std::wstring(o.GetNamedString(key)) : std::wstring();
}

// The sizes as start.json names them, smallest first.
constexpr const wchar_t* kSizeKeys[StartLayout::kSizes] = {L"small", L"medium", L"large", L"xlarge"};

int SizeOf(const std::wstring& name)
{
    for (int i = 0; i < StartLayout::kSizes; ++i)
        if (name == kSizeKeys[i])
            return i;
    return StartLayout::kMedium;
}

// GB2312 orders its level-one characters by pinyin: where each initial's run starts.
constexpr struct {
    unsigned code;
    wchar_t letter;
} kPinyin[] = {{0xB0A1, L'a'}, {0xB0C5, L'b'}, {0xB2C1, L'c'}, {0xB4EE, L'd'}, {0xB6EA, L'e'}, {0xB7A2, L'f'}, {0xB8C1, L'g'}, {0xB9FE, L'h'},
               {0xBBF7, L'j'}, {0xBFA6, L'k'}, {0xC0AC, L'l'}, {0xC2E8, L'm'}, {0xC4C3, L'n'}, {0xC5B6, L'o'}, {0xC5BE, L'p'}, {0xC6DA, L'q'},
               {0xC8BB, L'r'}, {0xC8F6, L's'}, {0xCBFA, L't'}, {0xCDDA, L'w'}, {0xCEF4, L'x'}, {0xD1B9, L'y'}, {0xD4D1, L'z'}};
constexpr unsigned kPinyinEnd = 0xD7FA;

// The pinyin initial of a Chinese character, or 0.
wchar_t PinyinInitial(wchar_t ch)
{
    if (ch < 0x4E00 || ch > 0x9FFF)
        return 0;
    char bytes[4] = {};
    BOOL lossy = FALSE;
    if (WideCharToMultiByte(936, 0, &ch, 1, bytes, 4, nullptr, &lossy) != 2 || lossy)
        return 0;
    const unsigned code = (unsigned(BYTE(bytes[0])) << 8) | BYTE(bytes[1]);
    if (code < kPinyin[0].code || code >= kPinyinEnd)
        return 0; // level two: ordered by radical, not by sound
    wchar_t letter = 0;
    for (const auto& p : kPinyin)
        if (code >= p.code)
            letter = p.letter;
    return letter;
}

} // namespace

void StartLayout::Clamp()
{
    size = std::clamp(size, 0, kSizes - 1);
    icon = std::clamp(icon, 0, kSizes - 1);
    // A folder keeps at least one app; an app appears once.
    std::vector<std::wstring> seen;
    auto fresh = [&](const std::wstring& id) {
        if (id.empty() || std::ranges::find(seen, id) != seen.end())
            return false;
        seen.push_back(id);
        return true;
    };
    std::vector<StartTile> kept;
    for (StartTile& t : tiles) {
        if (t.Folder()) {
            std::erase_if(t.apps, [&](const StartTile& a) { return a.Folder() || !fresh(a.id); });
            if (!t.apps.empty() && fresh(t.id))
                kept.push_back(std::move(t));
        } else if (fresh(t.id)) {
            t.apps.clear();
            kept.push_back(std::move(t));
        }
    }
    tiles = std::move(kept);
    std::vector<std::wstring> once;
    for (std::wstring& id : hidden)
        if (!id.empty() && std::ranges::find(once, id) == once.end())
            once.push_back(std::move(id));
    hidden = std::move(once);
}

std::wstring StartLayout::NewFolderId() const
{
    unsigned long next = 1;
    for (const StartTile& t : tiles)
        if (t.Folder())
            next = std::max(next, wcstoul(t.id.c_str() + 7, nullptr, 10) + 1);
    return L"folder:" + std::to_wstring(next);
}

bool ParseStartLayoutJson(std::wstring_view text, StartLayout& layout)
{
    layout = {};
    json::JsonObject o{nullptr};
    if (text.empty() || !json::JsonObject::TryParse(text, o))
        return false;
    const std::wstring size = Str(o, L"size");
    layout.size = SizeOf(size);
    layout.icon = SizeOf(Str(o, L"icon"));
    if (o.GetNamedNumber(L"version", 1) < 2) {
        // Version 1: sizes a step smaller (5 x 3, 7 x 3, 9 x 4), or before them the tiles across and down.
        if (!size.empty())
            layout.size = size == L"large" ? 1 : 0;
        else if (o.HasKey(L"cols") && o.HasKey(L"rows"))
            layout.size = o.GetNamedNumber(L"cols", 7) >= 9 || o.GetNamedNumber(L"rows", 3) >= 4 ? 1 : 0;
        layout.icon = StartLayout::kMedium; // the icons had no size of their own then
    }
    auto tile = [](const json::JsonObject& e) {
        StartTile t{Str(e, L"id"), Str(e, L"name"), {}, Str(e, L"alias")};
        if (e.HasKey(L"apps") && e.GetNamedValue(L"apps").ValueType() == json::JsonValueType::Array)
            for (const auto& v : e.GetNamedArray(L"apps"))
                if (v.ValueType() == json::JsonValueType::Object) {
                    const auto a = v.GetObject();
                    t.apps.push_back({Str(a, L"id"), Str(a, L"name"), {}, Str(a, L"alias")});
                }
        return t;
    };
    if (o.HasKey(L"tiles") && o.GetNamedValue(L"tiles").ValueType() == json::JsonValueType::Array) {
        layout.custom = true;
        for (const auto& v : o.GetNamedArray(L"tiles"))
            if (v.ValueType() == json::JsonValueType::Object)
                layout.tiles.push_back(tile(v.GetObject()));
    }
    if (o.HasKey(L"hidden") && o.GetNamedValue(L"hidden").ValueType() == json::JsonValueType::Array)
        for (const auto& v : o.GetNamedArray(L"hidden"))
            if (v.ValueType() == json::JsonValueType::String)
                layout.hidden.emplace_back(v.GetString());
    if (o.HasKey(L"windowsPins") && o.GetNamedValue(L"windowsPins").ValueType() == json::JsonValueType::Array) {
        layout.windowsPinsKnown = true;
        for (const auto& v : o.GetNamedArray(L"windowsPins"))
            if (v.ValueType() == json::JsonValueType::String)
                layout.windowsPins.emplace_back(v.GetString());
    }
    layout.Clamp();
    return true;
}

StartLayout LoadStartLayout()
{
    StartLayout layout;
    std::wstring text = ReadTextFile(LayoutPath());
    if (!text.empty() && !ParseStartLayoutJson(text, layout))
        Log(L"start: %s is not valid JSON, using the Windows pins", LayoutPath().c_str());
    return layout;
}

std::wstring SerializeStartLayoutJson(const StartLayout& layout)
{
    json::JsonObject o;
    o.SetNamedValue(L"version", json::JsonValue::CreateNumberValue(2));
    o.SetNamedValue(L"size", json::JsonValue::CreateStringValue(kSizeKeys[layout.size]));
    o.SetNamedValue(L"icon", json::JsonValue::CreateStringValue(kSizeKeys[layout.icon]));
    auto entry = [](const StartTile& t) {
        json::JsonObject e;
        e.SetNamedValue(L"id", json::JsonValue::CreateStringValue(t.id));
        e.SetNamedValue(L"name", json::JsonValue::CreateStringValue(t.name));
        if (!t.alias.empty())
            e.SetNamedValue(L"alias", json::JsonValue::CreateStringValue(t.alias));
        return e;
    };
    if (layout.custom) {
        json::JsonArray tiles;
        for (const StartTile& t : layout.tiles) {
            json::JsonObject e = entry(t);
            if (t.Folder()) {
                json::JsonArray apps;
                for (const StartTile& a : t.apps)
                    apps.Append(entry(a));
                e.SetNamedValue(L"apps", apps);
            }
            tiles.Append(e);
        }
        o.SetNamedValue(L"tiles", tiles);
    }
    if (layout.custom && layout.windowsPinsKnown) {
        json::JsonArray pins;
        for (const std::wstring& id : layout.windowsPins)
            pins.Append(json::JsonValue::CreateStringValue(id));
        o.SetNamedValue(L"windowsPins", pins);
    }
    if (!layout.hidden.empty()) {
        json::JsonArray hidden;
        for (const std::wstring& id : layout.hidden)
            hidden.Append(json::JsonValue::CreateStringValue(id));
        o.SetNamedValue(L"hidden", hidden);
    }
    return std::wstring(o.Stringify());
}

bool SaveStartLayout(const StartLayout& layout)
{
    if (WriteTextFile(LayoutPath(), SerializeStartLayoutJson(layout)))
        return true;
    Log(L"start: could not save %s", LayoutPath().c_str());
    return false;
}

std::wstring NameInitials(const std::wstring& name)
{
    std::wstring out;
    bool wordStart = true;
    for (size_t i = 0; i < name.size(); ++i) {
        const wchar_t ch = name[i];
        if (wchar_t py = PinyinInitial(ch)) {
            out += py;
            wordStart = true;
        } else if (iswalnum(ch)) {
            // A word's first letter, and a capital inside one ("OneDrive" -> "od").
            if (wordStart || (iswupper(ch) && i > 0 && iswlower(name[i - 1])))
                out += wchar_t(towlower(ch));
            wordStart = false;
        } else {
            wordStart = true;
        }
    }
    return out;
}

wchar_t IndexLetter(const std::wstring& name)
{
    for (wchar_t ch : name) {
        if (iswspace(ch))
            continue;
        if (ch < 128 && iswalpha(ch))
            return wchar_t(towupper(ch));
        if (ch >= 0xFF21 && ch <= 0xFF5A && iswalpha(ch)) // full-width Latin
            return wchar_t(towupper(wchar_t(ch - 0xFEE0)));
        if (wchar_t py = PinyinInitial(ch))
            return wchar_t(towupper(py));
        return L'#';
    }
    return L'#';
}

namespace {

uint32_t UnescapeEntity(std::wstring_view e)
{
    if (uint32_t cp = e == L"amp" ? L'&' : e == L"lt" ? L'<' : e == L"gt" ? L'>' : e == L"quot" ? L'"' : e == L"apos" ? L'\'' : 0)
        return cp;
    if (e.size() < 2 || e[0] != L'#')
        return 0;
    const bool hex = e[1] == L'x' || e[1] == L'X';
    const std::wstring_view digits = e.substr(hex ? 2 : 1);
    if (digits.empty())
        return 0;
    uint32_t v = 0;
    for (wchar_t ch : digits) {
        uint32_t d = 16;
        if (ch >= L'0' && ch <= L'9')
            d = uint32_t(ch - L'0');
        else if (hex && ch >= L'a' && ch <= L'f')
            d = uint32_t(ch - L'a' + 10);
        else if (hex && ch >= L'A' && ch <= L'F')
            d = uint32_t(ch - L'A' + 10);
        else
            return 0;
        v = v * (hex ? 16u : 10u) + d;
        if (v > 0x10FFFFu)
            return 0;
    }
    return v;
}

std::wstring UnescapeXml(std::wstring_view s)
{
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        size_t semi = s[i] == L'&' ? s.find(L';', i + 1) : std::wstring_view::npos;
        uint32_t cp = semi != std::wstring_view::npos && semi - i <= 12 ? UnescapeEntity(s.substr(i + 1, semi - i - 1)) : 0;
        if (!cp)
            out += s[i];
        else if (cp <= 0xFFFF)
            out += wchar_t(cp), i = semi;
        else
            cp -= 0x10000, out += wchar_t(0xD800 | (cp >> 10)), out += wchar_t(0xDC00 | (cp & 0x3FF)), i = semi;
    }
    return out;
}

struct XmlItem {
    int row = 0, col = 0;
    bool folder = false;
    std::wstring name;
    StartPinItem pin;
    std::vector<XmlItem> kids;
};

std::wstring_view LocalXmlName(std::wstring_view q)
{
    const size_t c = q.rfind(L':');
    return c == std::wstring_view::npos ? q : q.substr(c + 1);
}

void ApplyXmlAttr(std::wstring_view attr, std::wstring val, XmlItem& a)
{
    if (attr == L"Row")
        a.row = int(std::wcstol(val.c_str(), nullptr, 10));
    else if (attr == L"Column")
        a.col = int(std::wcstol(val.c_str(), nullptr, 10));
    else if (attr == L"Name")
        a.name = std::move(val);
    else if (attr == L"AppUserModelID")
        a.pin = {StartPinItem::Kind::PackagedAppId, std::move(val)};
    else if (attr == L"DesktopApplicationLinkPath")
        a.pin = {StartPinItem::Kind::DesktopAppLink, std::move(val)};
    else if (attr == L"DesktopApplicationID" && a.pin.value.empty())
        a.pin = {StartPinItem::Kind::DesktopAppId, std::move(val)};
}

bool ParseXmlTag(std::wstring_view xml, size_t& i, std::wstring_view& tag, bool& closing, bool& selfClosing, XmlItem& a)
{
    closing = i < xml.size() && xml[i] == L'/';
    if (closing)
        ++i;
    const size_t n = i;
    while (i < xml.size() && !iswspace(xml[i]) && xml[i] != L'>' && xml[i] != L'/')
        ++i;
    tag = LocalXmlName(xml.substr(n, i - n));
    if (closing) {
        const size_t e = xml.find(L'>', i);
        if (e == std::wstring_view::npos)
            return false;
        i = e + 1;
        return true;
    }
    while (i < xml.size()) {
        while (i < xml.size() && iswspace(xml[i]))
            ++i;
        if (i >= xml.size() || xml[i] == L'>' || (xml[i] == L'/' && i + 1 < xml.size() && xml[i + 1] == L'>')) {
            selfClosing = i < xml.size() && xml[i] == L'/';
            if (i < xml.size())
                i += 1 + size_t(selfClosing);
            return true;
        }
        const size_t k = i;
        while (i < xml.size() && !iswspace(xml[i]) && xml[i] != L'=' && xml[i] != L'>' && xml[i] != L'/')
            ++i;
        const std::wstring_view attr = LocalXmlName(xml.substr(k, i - k));
        while (i < xml.size() && (iswspace(xml[i]) || xml[i] == L'='))
            ++i;
        if (i >= xml.size() || (xml[i] != L'"' && xml[i] != L'\''))
            continue;
        const wchar_t q = xml[i++];
        const size_t ve = xml.find(q, i);
        if (ve == std::wstring_view::npos)
            return false;
        ApplyXmlAttr(attr, UnescapeXml(xml.substr(i, ve - i)), a);
        i = ve + 1;
    }
    return true;
}

bool SkipXmlNonTag(std::wstring_view xml, size_t i, size_t& next)
{
    const std::wstring_view rest = xml.substr(i);
    const bool isComment = rest.starts_with(L"!--");
    const bool isCdata = rest.starts_with(L"![CDATA[");
    if (!isComment && !isCdata && (rest.empty() || (rest[0] != L'?' && rest[0] != L'!')))
        return false;
    const wchar_t* end = isComment ? L"-->" : isCdata ? L"]]>" : L">";
    const size_t p = xml.find(end, i + 1);
    next = p == std::wstring_view::npos ? std::wstring_view::npos : p + ((isComment || isCdata) ? 3 : 1);
    return true;
}

void FlushXmlGroup(std::vector<StartPinGroup>& out, std::wstring& groupName, std::vector<XmlItem>& groupItems)
{
    auto byPos = [](const XmlItem& a, const XmlItem& b) { return a.row != b.row ? a.row < b.row : a.col < b.col; };
    std::ranges::stable_sort(groupItems, byPos);
    StartPinGroup group{std::move(groupName), false, {}};
    std::vector<StartPinGroup> promoted;
    for (XmlItem& item : groupItems) {
        if (!item.folder) {
            group.items.push_back(std::move(item.pin));
            continue;
        }
        std::ranges::stable_sort(item.kids, byPos);
        StartPinGroup fg{std::move(item.name), true, {}};
        for (XmlItem& c : item.kids)
            fg.items.push_back(std::move(c.pin));
        if (!group.name.empty()) {
            promoted.push_back(std::move(fg));
        } else {
            if (!group.items.empty()) {
                out.push_back(std::move(group));
                group = {};
            }
            out.push_back(std::move(fg));
        }
    }
    if (!group.items.empty())
        out.push_back(std::move(group));
    out.insert(out.end(), std::make_move_iterator(promoted.begin()), std::make_move_iterator(promoted.end()));
    groupItems.clear();
    groupName.clear();
}

} // namespace

std::vector<StartPinGroup> ParseStartLayoutXml(std::wstring_view xml)
{
    if (xml.starts_with(L'\xFEFF'))
        xml.remove_prefix(1);
    std::vector<StartPinGroup> out;
    bool inGroup = false, inFolder = false;
    std::wstring groupName;
    std::vector<XmlItem> groupItems;
    XmlItem folderItem;
    auto closeFolder = [&] {
        if (inFolder && !folderItem.kids.empty())
            groupItems.push_back(std::move(folderItem));
        folderItem = {};
        inFolder = false;
    };
    auto finishGroup = [&] {
        closeFolder();
        if (inGroup)
            FlushXmlGroup(out, groupName, groupItems);
        inGroup = false;
    };
    for (size_t lt = xml.find(L'<'); lt != std::wstring_view::npos; lt = xml.find(L'<', lt)) {
        size_t i = lt + 1;
        if (size_t next = 0; SkipXmlNonTag(xml, i, next)) {
            if (next == std::wstring_view::npos)
                break;
            lt = next;
            continue;
        }
        std::wstring_view tag;
        bool closing = false, selfClosing = false;
        XmlItem a;
        if (!ParseXmlTag(xml, i, tag, closing, selfClosing, a))
            break;
        lt = i;
        if (closing && tag == L"Group") {
            finishGroup();
        } else if (closing && tag == L"Folder") {
            closeFolder();
        } else if (!closing && tag == L"Group") {
            finishGroup();
            if (!selfClosing) {
                inGroup = true;
                groupName = std::move(a.name);
            }
        } else if (!closing && tag == L"Folder" && inGroup && !inFolder && !selfClosing) {
            inFolder = true;
            a.folder = true;
            folderItem = std::move(a);
        } else if (!closing && inGroup && (tag == L"Tile" || tag == L"DesktopApplicationTile") && !a.pin.value.empty()) {
            (inFolder ? folderItem.kids : groupItems).push_back(std::move(a));
        }
    }
    finishGroup();
    return out;
}

bool RemoveStartFolder(std::vector<StartTile>& tiles, const std::wstring& folderId)
{
    return std::erase_if(tiles, [&](const StartTile& t) { return t.Folder() && t.id == folderId; }) > 0;
}

bool UngroupStartFolder(std::vector<StartTile>& tiles, const std::wstring& folderId)
{
    auto at = std::ranges::find_if(tiles, [&](const StartTile& t) { return t.Folder() && t.id == folderId; });
    if (at == tiles.end())
        return false;
    auto apps = std::move(at->apps);
    return tiles.insert(tiles.erase(at), std::make_move_iterator(apps.begin()), std::make_move_iterator(apps.end())), true;
}

bool ApplyTileAlias(std::vector<StartTile>& tiles, const std::wstring& id, const std::wstring& alias)
{
    for (StartTile& t : tiles) {
        if (!t.Folder()) {
            if (t.id == id) {
                t.alias = alias;
                return true;
            }
        } else {
            for (StartTile& a : t.apps) {
                if (!a.Folder() && a.id == id) {
                    a.alias = alias;
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace atrium
