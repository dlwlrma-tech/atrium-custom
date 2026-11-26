#include "shell/launchpad_order.h"

#include "log.h"
#include "settings.h"

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

namespace json = winrt::Windows::Data::Json;

namespace atrium {

namespace {

std::wstring OrderPath()
{
    std::wstring path = SettingsPath(); // ...\Atrium\settings.json
    return path.substr(0, path.find_last_of(L'\\') + 1) + L"launchpad.json";
}

} // namespace

std::vector<std::wstring> LoadLaunchpadOrder()
{
    std::vector<std::wstring> ids;
    const std::wstring text = ReadTextFile(OrderPath());
    json::JsonObject o{nullptr};
    if (text.empty() || !json::JsonObject::TryParse(text, o) || !o.HasKey(L"order") || o.GetNamedValue(L"order").ValueType() != json::JsonValueType::Array)
        return ids;
    for (const auto& v : o.GetNamedArray(L"order"))
        if (v.ValueType() == json::JsonValueType::String && ids.size() < 4096)
            ids.emplace_back(v.GetString());
    return ids;
}

void SaveLaunchpadOrder(const std::vector<std::wstring>& ids)
{
    json::JsonArray order;
    for (const std::wstring& id : ids)
        order.Append(json::JsonValue::CreateStringValue(id));
    json::JsonObject o;
    o.SetNamedValue(L"order", order);
    if (!WriteTextFile(OrderPath(), std::wstring(o.Stringify())))
        Log(L"launchpad: could not save %s", OrderPath().c_str());
}

} // namespace atrium
