#include "pch.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

struct IniSection
{
    std::string name;
    std::vector<std::pair<std::string, std::string>> values;
};

struct VersionRule
{
    std::uintptr_t offset = 0;
    bool raw = false;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> signatures;
};

struct PatchContext
{
    HMODULE exeBase = nullptr;
    std::filesystem::path iniDirectory;
    std::filesystem::path logPath;
    std::map<std::string, std::vector<std::uint8_t>> font;
    std::optional<std::string> version;
    std::vector<std::string> versionOffsetKeys;
};

struct PatchReport
{
    size_t applied = 0;
    std::vector<std::string> failures;
};

std::string Trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right)
{
    return ToLower(std::string(left)) == ToLower(std::string(right));
}

std::vector<std::string> SplitComma(const std::string& value)
{
    std::vector<std::string> items;
    std::stringstream stream(value);
    std::string item;
    while (std::getline(stream, item, ','))
    {
        item = Trim(item);
        if (!item.empty())
        {
            items.push_back(item);
        }
    }
    return items;
}

std::optional<std::uintptr_t> ParseInteger(const std::string& value)
{
    try
    {
        size_t parsed = 0;
        const auto result = std::stoull(Trim(value), &parsed, 0);
        return parsed == Trim(value).size() ? std::optional<std::uintptr_t>(static_cast<std::uintptr_t>(result)) : std::nullopt;
    }
    catch (...)
    {
        return std::nullopt;
    }
}

bool ParseBool(const std::string& value)
{
    const auto lower = ToLower(Trim(value));
    return lower == "true" || lower == "1" || lower == "yes" || lower == "on";
}

std::optional<std::vector<std::uint8_t>> ParseHexBytes(const std::string& value)
{
    std::vector<std::uint8_t> bytes;
    std::stringstream stream(value);
    std::string token;
    while (stream >> token)
    {
        if (token.starts_with("0x") || token.starts_with("0X"))
        {
            token = token.substr(2);
        }
        if (token.empty() || token.size() > 2)
        {
            return std::nullopt;
        }
        try
        {
            bytes.push_back(static_cast<std::uint8_t>(std::stoul(token, nullptr, 16)));
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
    return bytes;
}

std::vector<IniSection> ParseIni(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::vector<IniSection> sections;
    IniSection* current = nullptr;
    std::string line;

    while (std::getline(file, line))
    {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
        {
            continue;
        }

        if (line.front() == '[' && line.back() == ']')
        {
            sections.push_back({ Trim(line.substr(1, line.size() - 2)), {} });
            current = &sections.back();
            continue;
        }

        const auto equals = line.find('=');
        if (current == nullptr || equals == std::string::npos)
        {
            continue;
        }

        current->values.emplace_back(Trim(line.substr(0, equals)), Trim(line.substr(equals + 1)));
    }

    return sections;
}

std::optional<std::string> GetValue(const IniSection& section, std::string_view key)
{
    for (auto it = section.values.rbegin(); it != section.values.rend(); ++it)
    {
        if (EqualsIgnoreCase(it->first, key))
        {
            return it->second;
        }
    }
    return std::nullopt;
}
    
std::uintptr_t RawToRva(HMODULE module, std::uintptr_t raw)
{
    const auto base = reinterpret_cast<std::uint8_t*>(module);
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto sections = IMAGE_FIRST_SECTION(nt);

    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index)
    {
        const auto& section = sections[index];
        const auto start = static_cast<std::uintptr_t>(section.PointerToRawData);
        const auto size = static_cast<std::uintptr_t>((std::max)(section.SizeOfRawData, section.Misc.VirtualSize));
        if (raw >= start && raw < start + size)
        {
            return static_cast<std::uintptr_t>(section.VirtualAddress) + (raw - start);
        }
    }

    return raw;
}

std::uintptr_t ResolveRva(const PatchContext& context, std::uintptr_t offset, bool raw)
{
    return raw ? RawToRva(context.exeBase, offset) : offset;
}

bool WriteMemory(void* address, const void* data, size_t size)
{
    DWORD oldProtect = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        return false;
    }

    std::memcpy(address, data, size);
    FlushInstructionCache(GetCurrentProcess(), address, size);

    DWORD unused = 0;
    VirtualProtect(address, size, oldProtect, &unused);
    return true;
}

std::vector<std::uint8_t> EncodeString(const std::string& value, const std::map<std::string, std::vector<std::uint8_t>>& font)
{
    std::vector<std::uint8_t> output;
    for (size_t index = 0; index < value.size();)
    {
        const auto match = std::find_if(font.begin(), font.end(), [&](const auto& item) {
            return value.compare(index, item.first.size(), item.first) == 0;
        });

        if (match != font.end())
        {
            output.insert(output.end(), match->second.begin(), match->second.end());
            index += match->first.size();
        }
        else
        {
            output.push_back(static_cast<std::uint8_t>(value[index]));
            ++index;
        }
    }
    return output;
}

std::optional<std::vector<std::uint8_t>> BuildData(const IniSection& section, const PatchContext& context)
{
    const auto type = GetValue(section, "Type");
    const auto data = GetValue(section, "Data");
    if (!type || !data)
    {
        return std::nullopt;
    }

    const auto lowerType = ToLower(*type);
    std::optional<std::vector<std::uint8_t>> bytes;
    if (lowerType == "string")
    {
        bytes = EncodeString(*data, context.font);
    }
    else if (lowerType == "hex")
    {
        bytes = ParseHexBytes(*data);
    }
    else if (lowerType == "file")
    {
        std::ifstream file(context.iniDirectory / *data, std::ios::binary);
        if (!file)
        {
            return std::nullopt;
        }
        bytes = std::vector<std::uint8_t>(std::istreambuf_iterator<char>(file), {});
    }
    else
    {
        return std::nullopt;
    }

    if (bytes && (bytes->empty() || bytes->back() != 0x00))
    {
        bytes->push_back(0x00);
    }

    return bytes;
}

bool ApplyPatch(const IniSection& section, const PatchContext& context, std::string& failureReason)
{
    const auto data = BuildData(section, context);
    if (!data || data->empty())
    {
        failureReason = "missing or invalid Type/Data";
        return false;
    }

    const auto rawValue = GetValue(section, "Raw");
    const auto pointerValue = GetValue(section, "Pointer");
    const bool raw = rawValue ? ParseBool(*rawValue) : false;
    const bool pointer = pointerValue ? ParseBool(*pointerValue) : false;
    std::vector<std::string> offsets;

    auto offsetValue = GetValue(section, "Offsets");
    if (offsetValue)
    {
        auto parsedOffsets = SplitComma(*offsetValue);
        offsets.insert(offsets.end(), parsedOffsets.begin(), parsedOffsets.end());
    }
    
    if (context.version)
    {
        for (const auto& key : context.versionOffsetKeys)
        {
            const auto versioned = GetValue(section, key);
            if (versioned)
            {
                auto parsedOffsets = SplitComma(*versioned);
                offsets.insert(offsets.end(), parsedOffsets.begin(), parsedOffsets.end());
            }
        }
    }

    if (offsets.empty())
    {
        failureReason = "no matching Offsets";
        return false;
    }

    std::vector<std::uint8_t> pointerBytes;
    const std::vector<std::uint8_t>* bytesToWrite = &*data;
    if (pointer)
    {
        void* allocated = VirtualAlloc(nullptr, data->size(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (allocated == nullptr)
        {
            failureReason = "VirtualAlloc failed";
            return false;
        }
        std::memcpy(allocated, data->data(), data->size());

        const auto address = reinterpret_cast<std::uintptr_t>(allocated);
        pointerBytes.resize(sizeof(address));
        std::memcpy(pointerBytes.data(), &address, sizeof(address));
        bytesToWrite = &pointerBytes;
    }

    const auto base = reinterpret_cast<std::uint8_t*>(context.exeBase);
    for (const auto& item : offsets)
    {
        const auto parsed = ParseInteger(item);
        if (!parsed)
        {
            failureReason = "invalid offset: " + item;
            return false;
        }
        const auto rva = ResolveRva(context, *parsed, raw);
        if (!WriteMemory(base + rva, bytesToWrite->data(), bytesToWrite->size()))
        {
            failureReason = "WriteMemory failed at " + item;
            return false;
        }
    }

    return true;
}

void WriteLog(const PatchContext& context, const PatchReport& report, double elapsedMilliseconds)
{
    std::ofstream log(context.logPath, std::ios::trunc);
    if (!log)
    {
        return;
    }

    log << "Patches applied: " << report.applied << '\n';
    log << "Load time: " << elapsedMilliseconds << " ms\n";
    log << "Failed patches: " << report.failures.size() << '\n';

    for (const auto& failure : report.failures)
    {
        log << "- " << failure << '\n';
    }
}

std::filesystem::path GetModuleSidecarPath(HMODULE dllModule, const wchar_t* extension)
{
    wchar_t dllPath[MAX_PATH] = {};
    GetModuleFileNameW(dllModule, dllPath, MAX_PATH);
    auto path = std::filesystem::path(dllPath);
    path.replace_extension(extension);
    return path;
}

std::string PatchName(const IniSection& section)
{
    return section.name.empty() ? "<unnamed>" : section.name;
}

void ApplyPatches(const std::vector<IniSection>& sections, const PatchContext& context, PatchReport& report)
{
    for (const auto& section : sections)
    {
        if (EqualsIgnoreCase(section.name, "Font") || EqualsIgnoreCase(section.name, "Different_Versions"))
        {
            continue;
        }

        std::string failureReason;
        if (ApplyPatch(section, context, failureReason))
        {
            ++report.applied;
        }
        else
        {
            report.failures.push_back(PatchName(section) + ": " + failureReason);
        }
    }
}

std::optional<VersionRule> ParseVersionRule(const IniSection& section)
{
    const auto offsetValue = GetValue(section, "Offsets");
    std::optional<std::uintptr_t> offset;
    if (offsetValue)
    {
        offset = ParseInteger(*offsetValue);
    }
    if (!offset)
    {
        return std::nullopt;
    }

    VersionRule rule;
    rule.offset = *offset;
    const auto rawValue = GetValue(section, "Raw");
    rule.raw = rawValue ? ParseBool(*rawValue) : false;

    for (const auto& [key, value] : section.values)
    {
        if (EqualsIgnoreCase(key, "Offsets") || EqualsIgnoreCase(key, "Raw"))
        {
            continue;
        }
        auto bytes = ParseHexBytes(value);
        if (bytes && !bytes->empty())
        {
            rule.signatures.emplace_back(key, *bytes);
        }
    }

    return rule;
}

std::optional<std::string> DetectVersion(const VersionRule& rule, const PatchContext& context)
{
    const auto base = reinterpret_cast<std::uint8_t*>(context.exeBase);
    const auto rva = ResolveRva(context, rule.offset, rule.raw);

    for (const auto& [name, signature] : rule.signatures)
    {
        if (std::memcmp(base + rva, signature.data(), signature.size()) == 0)
        {
            return name;
        }
    }
    return std::nullopt;
}

void SetDetectedVersion(const VersionRule& rule, PatchContext& context)
{
    const auto detected = DetectVersion(rule, context);
    if (!detected)
    {
        return;
    }

    context.version = *detected;
    context.versionOffsetKeys.push_back("Offsets_" + *detected);

    for (const auto& [name, signature] : rule.signatures)
    {
        if (name == *detected && !signature.empty())
        {
            char alias[16] = {};
            sprintf_s(alias, "Offsets_%02X", signature.front());
            context.versionOffsetKeys.emplace_back(alias);
            break;
        }
    }
}

void ParseFont(const IniSection& section, PatchContext& context)
{
    for (const auto& [key, value] : section.values)
    {
        auto bytes = ParseHexBytes(value);
        if (bytes)
        {
            context.font[key] = *bytes;
        }
    }
}

std::filesystem::path FindIniPath(HMODULE dllModule)
{
    return GetModuleSidecarPath(dllModule, L".ini");
}

DWORD WINAPI PatchThread(LPVOID parameter)
{
    const auto start = std::chrono::steady_clock::now();
    const auto dllModule = static_cast<HMODULE>(parameter);
    const auto iniPath = FindIniPath(dllModule);
    PatchReport report;
    PatchContext context;
    context.exeBase = GetModuleHandleW(nullptr);
    context.iniDirectory = iniPath.parent_path();
    context.logPath = GetModuleSidecarPath(dllModule, L".log");

    if (!std::filesystem::exists(iniPath))
    {
        report.failures.push_back("INI file not found: " + iniPath.filename().string());
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        WriteLog(context, report, elapsed);
        return 0;
    }

    auto sections = ParseIni(iniPath);
    for (const auto& section : sections)
    {
        if (EqualsIgnoreCase(section.name, "Font"))
        {
            ParseFont(section, context);
        }
    }

    for (const auto& section : sections)
    {
        if (EqualsIgnoreCase(section.name, "Different_Versions"))
        {
            auto rule = ParseVersionRule(section);
            if (rule)
            {
                SetDetectedVersion(*rule, context);
            }
            break;
        }
    }

    ApplyPatches(sections, context, report);

    const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    WriteLog(context, report, elapsed);

    return 0;
}

}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        HANDLE thread = CreateThread(nullptr, 0, PatchThread, hModule, 0, nullptr);
        if (thread != nullptr)
        {
            CloseHandle(thread);
        }
    }
    return TRUE;
}

