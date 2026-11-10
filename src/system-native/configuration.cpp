#include "configuration.h"
#include <filesystem>
#include <cmath>
#include <shobjidl.h>
namespace magic::system
{
namespace
{
constexpr wchar_t ConfigProtocol[] = L"Magic.System.Configuration/1";
constexpr DWORD ConfigLimit = 256 * 1024;
constexpr wchar_t const *Modules[] = {
    L"magic.user",        L"magic.power-menu",   L"magic.clock",   L"magic.tray",           L"magic.bluetooth",
    L"magic.keyboard",    L"magic.battery",      L"magic.network", L"magic.quick-settings", L"magic.notifications",
    L"magic.audio",       L"magic.player",       L"magic.start",   L"magic.desktop",        L"magic.recycle",
    L"magic.cpu",         L"magic.memory",       L"magic.gpu",     L"magic.storage",        L"magic.network-usage",
    L"magic.focused-app", L"magic.focused-title", L"magic.lyrics", L"magic.music"};
void BoundedText(JsonObject const &object, wchar_t const *key, size_t max, bool optional = false)
{
    if (optional && !object.HasKey(key))
        return;
    auto value = object.GetNamedString(key);
    if (value.size() > max || std::wstring(value).find(L'\0') != std::wstring::npos)
        throw std::invalid_argument("Configuration string exceeds bound");
}
void Keys(JsonObject const &object, std::set<std::wstring> const &allowed)
{
    for (auto const &entry : object)
        if (!allowed.contains(std::wstring(entry.Key())))
            throw std::invalid_argument("Unknown configuration key");
}
void StyleValue(IJsonValue const &value, unsigned depth)
{
    if (depth > 2)
        throw std::invalid_argument("Appearance nesting exceeds bound");
    if (value.ValueType() == JsonValueType::Object)
    {
        auto object = value.GetObject();
        if (object.Size() > 32)
            throw std::invalid_argument("Appearance property count exceeds bound");
        for (auto const &field : object)
        {
            if (field.Key().size() > 64)
                throw std::invalid_argument("Appearance key too long");
            StyleValue(field.Value(), depth + 1);
        }
    }
    else if (value.ValueType() == JsonValueType::String)
    {
        if (value.GetString().size() > 128)
            throw std::invalid_argument("Appearance text too long");
    }
    else if (value.ValueType() == JsonValueType::Number)
    {
        auto n = value.GetNumber();
        if (n < -4096 || n > 60000)
            throw std::invalid_argument("Appearance number outside bounds");
    }
    else if (value.ValueType() != JsonValueType::Boolean && value.ValueType() != JsonValueType::Null)
        throw std::invalid_argument("Unsupported appearance value");
}
} // namespace
ConfigurationProvider::ConfigurationProvider(std::wstring root, std::shared_ptr<ChangeState> signal)
    : directory(std::move(root)), state(std::move(signal))
{
    if (!directory.empty())
        VerifyDirectory();
}
std::wstring ConfigurationProvider::File() const
{
    return directory + L"\\configuration.json";
}
void ConfigurationProvider::VerifyDirectory() const
{
    if (directory.size() < 3 || directory[1] != L':' || directory[2] != L'\\' || directory.starts_with(L"\\\\"))
        throw std::invalid_argument("Configuration requires a local absolute data directory");
    for (auto path = std::filesystem::path(directory); !path.empty();)
    {
        DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::invalid_argument("Configuration directory is missing or contains a reparse point");
        auto next = path.parent_path();
        if (next == path)
            break;
        path = next;
    }
}
JsonObject ConfigurationProvider::Load() const
{
    VerifyDirectory();
    Handle file{CreateFileW(File().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (file.value == INVALID_HANDLE_VALUE)
    {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND)
            return JsonObject{};
        winrt::throw_hresult(HRESULT_FROM_WIN32(error));
    }
    BY_HANDLE_FILE_INFORMATION info{};
    Check(GetFileInformationByHandle(file, &info) != FALSE);
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        throw std::invalid_argument("Configuration file cannot be a reparse point");
    LARGE_INTEGER size{};
    Check(GetFileSizeEx(file, &size) != FALSE);
    if (size.QuadPart <= 0 || size.QuadPart > ConfigLimit)
        throw std::invalid_argument("Configuration size exceeds bound");
    std::string bytes(size_t(size.QuadPart), '\0');
    DWORD read = 0;
    Check(ReadFile(file, bytes.data(), DWORD(bytes.size()), &read, nullptr) != FALSE);
    if (read != bytes.size())
        throw std::runtime_error("Configuration changed while reading");
    auto result = JsonObject::Parse(winrt::to_hstring(bytes));
    Validate(result);
    return result;
}
JsonObject ConfigurationProvider::Read() const
{
    JsonObject response;
    Flag(response, L"enabled", !directory.empty());
    if (directory.empty())
    {
        Text(response, L"reason", L"No data namespace selected; no configuration read or created");
        return response;
    }
    auto config = Load();
    Flag(response, L"present", config.Size() > 0);
    Number(response, L"revision", config.Size() ? config.GetNamedNumber(L"revision") : 0);
    if (config.Size())
        response.Insert(L"configuration", config);
    return response;
}
JsonObject ConfigurationProvider::LaunchTargets() const
{
    auto config = Load();
    JsonArray targets;
    if (config.Size())
        for (auto surface : {L"toolbar", L"dock"})
            for (auto section : {L"left", L"center", L"right"})
                for (auto const &value :
                     config.GetNamedObject(surface).GetNamedObject(L"sections").GetNamedArray(section))
                {
                    auto item = value.GetObject();
                    if (item.GetNamedString(L"kind", L"") != L"application")
                        continue;
                    std::wstring target(item.GetNamedString(L"target", L""));
                    auto launch = item.GetNamedObject(L"launch", JsonObject{});
                    if (auto executable = launch.GetNamedString(L"executable", L""); !executable.empty())
                        target = executable;
                    if (target.size() > 4 && _wcsicmp(target.substr(target.size() - 4).c_str(), L".lnk") == 0 &&
                        target.size() > 3 && target[1] == L':' && target[2] == L'\\')
                    {
                        wchar_t drive[]{target[0], L':', L'\\', 0};
                        if (GetDriveTypeW(drive) == DRIVE_FIXED)
                            try
                            {
                                winrt::com_ptr<IShellLinkW> link;
                                winrt::check_hresult(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                                                      __uuidof(IShellLinkW), link.put_void()));
                                winrt::check_hresult(link.as<IPersistFile>()->Load(target.c_str(), STGM_READ));
                                wchar_t path[32768]{};
                                if (SUCCEEDED(link->GetPath(path, 32768, nullptr, SLGP_RAWPATH)) && *path)
                                {
                                    wchar_t expanded[32768]{};
                                    DWORD count = ExpandEnvironmentStringsW(path, expanded, 32768);
                                    if (count && count < 32768)
                                        target = expanded;
                                }
                            }
                            catch (...)
                            {
                            }
                    }
                    JsonObject row;
                    Text(row, L"id", item.GetNamedString(L"id"));
                    Text(row, L"target", target);
                    targets.Append(row);
                }
    JsonObject result;
    result.Insert(L"items", targets);
    return result;
}
JsonObject ConfigurationProvider::Catalog()
{
    JsonObject result;
    Text(result, L"protocol", ConfigProtocol);
    Number(result, L"schemaVersion", 1);
    JsonArray modules;
    for (auto name : Modules)
    {
        JsonObject row;
        Text(row, L"id", name);
        modules.Append(row);
    }
    result.Insert(L"modules", modules);
    Flag(result, L"registrationOnly", true);
    Flag(result, L"compatibilityExecutionSupported", false);
    return result;
}
void ConfigurationProvider::Validate(JsonObject const &config)
{
    Keys(config, {L"protocol", L"schemaVersion", L"revision", L"toolbar", L"dock", L"preferences"});
    if (config.GetNamedString(L"protocol") != ConfigProtocol || config.GetNamedNumber(L"schemaVersion") != 1)
        throw std::invalid_argument("Unsupported configuration schema");
    double revision = config.GetNamedNumber(L"revision");
    if (revision < 1 || revision > 1000000000 || revision != double(uint64_t(revision)))
        throw std::invalid_argument("Invalid configuration revision");
    std::set<std::wstring> ids;
    size_t count = 0;
    for (auto surface : {L"toolbar", L"dock"})
    {
        auto bar = config.GetNamedObject(surface);
        if(std::wstring(surface)==L"dock")Keys(bar, {L"locked", L"sections", L"appearance", L"previewMotion"});
        else Keys(bar, {L"locked", L"sections", L"appearance"});
        if(bar.HasKey(L"previewMotion")){
            auto motion=bar.GetNamedObject(L"previewMotion");Keys(motion,{L"duration",L"scale",L"shift"});
            for(auto key:{L"duration",L"scale",L"shift"})if(motion.HasKey(key)){
                double n=motion.GetNamedNumber(key),low=std::wstring(key)==L"duration"?80:std::wstring(key)==L"scale"?.6:0,high=std::wstring(key)==L"duration"?600:std::wstring(key)==L"scale"?1:24;
                if(!std::isfinite(n)||n<low||n>high||(std::wstring(key)==L"duration"&&n!=std::floor(n)))throw std::invalid_argument("Preview motion value exceeds bound");
            }
        }
        bar.GetNamedBoolean(L"locked");
        auto sections = bar.GetNamedObject(L"sections");
        Keys(sections, {L"left", L"center", L"right"});
        for (auto section : {L"left", L"center", L"right"})
            for (auto const &value : sections.GetNamedArray(section))
            {
                if (++count > 256)
                    throw std::invalid_argument("Too many configured items");
                auto row = value.GetObject();
                Keys(row, {L"id", L"kind", L"module", L"displayName", L"target", L"aumid", L"pinned", L"allowPin",
                           L"launch", L"sourceModuleId"});
                auto id = RequiredText(row, L"id", 96);
                if (!ids.insert(id).second)
                    throw std::invalid_argument("Duplicate configuration item ID");
                auto kind = RequiredText(row, L"kind", 32);
                if (kind == L"module")
                {
                    auto module = RequiredText(row, L"module", 96);
                    if (std::find(std::begin(Modules), std::end(Modules), module) == std::end(Modules))
                        throw std::invalid_argument("Unknown native module ID");
                }
                else if (kind == L"application")
                {
                    BoundedText(row, L"displayName", 512);
                    BoundedText(row, L"target", 32768);
                    BoundedText(row, L"aumid", 512);
                    row.GetNamedBoolean(L"pinned");
                    row.GetNamedBoolean(L"allowPin");
                    if (row.HasKey(L"launch"))
                    {
                        auto launch = row.GetNamedObject(L"launch");
                        Keys(launch, {L"executable", L"arguments", L"workingDirectory", L"iconSource"});
                        BoundedText(launch, L"executable", 32768);
                        if (launch.HasKey(L"arguments"))
                        {
                            auto arguments = launch.GetNamedValue(L"arguments");
                            if (arguments.ValueType() == JsonValueType::Array)
                            {
                                auto values = arguments.GetArray();
                                if (values.Size() > 128)
                                    throw std::invalid_argument("Too many launch arguments");
                                for (auto const &item : values)
                                    if (item.GetString().size() > 8192)
                                        throw std::invalid_argument("Launch argument exceeds bound");
                            }
                            else
                                BoundedText(launch, L"arguments", 8192);
                        }
                        BoundedText(launch, L"workingDirectory", 32768, true);
                        BoundedText(launch, L"iconSource", 32768, true);
                    }
                }
                else if (kind == L"compatibility")
                    BoundedText(row, L"sourceModuleId", 256);
                else if (kind != L"separator")
                    throw std::invalid_argument("Unknown configuration item kind");
            }
        auto appearance = bar.GetNamedObject(L"appearance");
        Keys(appearance, {L"edge", L"enabled", L"autoHide", L"outerMargin", L"innerPadding", L"showDelayMs",
                          L"hideDelayMs", L"itemSize", L"iconSize", L"itemGap", L"widthMode", L"middleClickAction",
                          L"pinnedVisibility", L"runningVisibility", L"attentionReveal", L"showEndTask",
                          L"showInstanceCount", L"showWindowTitle", L"splitWindows",
                          // Appearance surface (accepted now, consumed by the renderer in P1):
                          // material = solid|acrylic|mica|glass|liquid
                          // fullscreenMaterial overrides material while a fullscreen window is focused.
                          L"material", L"tintColor", L"tintOpacity", L"blurRadius",
                          L"fullscreenMaterial", L"fullscreenColor", L"cornerRadius"});
        StyleValue(appearance, 0);
    }
    auto preferences = config.GetNamedObject(L"preferences");
    Keys(preferences, {L"language", L"dateFormat", L"reduceMotion"});
    BoundedText(preferences, L"language", 64, true);
    BoundedText(preferences, L"dateFormat", 128, true);
    if (preferences.HasKey(L"reduceMotion"))
        preferences.GetNamedBoolean(L"reduceMotion");
    if (winrt::to_string(config.Stringify()).size() > ConfigLimit)
        throw std::invalid_argument("Configuration size exceeds bound");
}
JsonObject ConfigurationProvider::Write(JsonObject const &args)
{
    if (directory.empty())
        throw std::invalid_argument("Configuration namespace was not selected");
    VerifyDirectory();
    Handle lock{CreateFileW((directory + L"\\configuration.lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    Check(lock.value != INVALID_HANDLE_VALUE);
    BY_HANDLE_FILE_INFORMATION lockInfo{};
    Check(GetFileInformationByHandle(lock, &lockInfo) != FALSE);
    if (lockInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        throw std::invalid_argument("Configuration lock cannot be a reparse point");
    auto next = args.GetNamedObject(L"configuration");
    Validate(next);
    double expected = args.GetNamedNumber(L"expectedRevision");
    auto prior = Load();
    double actual = prior.Size() ? prior.GetNamedNumber(L"revision") : 0;
    if (expected != actual || next.GetNamedNumber(L"revision") != actual + 1)
        throw std::invalid_argument("Configuration revision changed; reload before writing");
    GUID nonce{};
    winrt::check_hresult(CoCreateGuid(&nonce));
    wchar_t id[40]{};
    StringFromGUID2(nonce, id, 40);
    std::wstring temporary = directory + L"\\configuration-" + id + L".tmp";
    auto bytes = winrt::to_string(next.Stringify());
    {
        Handle file{CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr)};
        Check(file.value != INVALID_HANDLE_VALUE);
        DWORD written = 0;
        Check(WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr) != FALSE);
        if (written != bytes.size())
            throw std::runtime_error("Configuration write incomplete");
        Check(FlushFileBuffers(file) != FALSE);
    }
    VerifyDirectory();
    Check(MoveFileExW(temporary.c_str(), File().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE);
    state->Notify(32);
    JsonObject response;
    Flag(response, L"written", true);
    Number(response, L"revision", actual + 1);
    return response;
}
} // namespace magic::system
