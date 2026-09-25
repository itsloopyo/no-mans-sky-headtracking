#pragma once

// One reading of a config file as the differential test compares it, the same
// shape for the published build (the oracle, its own executable), the frozen
// import and the migration. Names are `status`, `field.<name>` for a field of
// the reader's Config, `startup.<name>` for the state the game starts in, and
// `hotkey.<action>` for the bindings it registers.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace nms_differential {

using Record = std::map<std::string, std::string>;

inline std::string Bits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(bits));
    return text;
}

inline std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%02X", static_cast<unsigned>(value));
    return text;
}

inline std::string Flag(bool value) { return value ? "1" : "0"; }

// A [Debug] caller list: `pinned` when the build profile's callers stay in
// force, otherwise the listed callers in order, `none` for a list of none.
inline std::string Callers(const std::optional<std::vector<std::uint32_t>>& callers) {
    if (!callers) return "pinned";
    if (callers->empty()) return "none";
    std::string text;
    for (const std::uint32_t rva : *callers) {
        if (!text.empty()) text += ',';
        text += Hex(rva);
    }
    return text;
}

// [Debug] SceneSampleRva: `profile` when the build profile's RVA stays in force.
inline std::string SceneSample(const std::optional<std::uint32_t>& rva) { return rva ? Hex(*rva) : "profile"; }

// Modifier bits as core's KeyModifiers numbers them: Ctrl 1, Shift 2, Alt 4.
constexpr unsigned kCtrlShift = 3;

// A binding list as `modifiers:code` items in ascending order, so two lists
// compare as sets: the order a list names its keys in never changes what fires.
inline std::string Bindings(std::vector<std::pair<unsigned, int>> items) {
    std::sort(items.begin(), items.end());
    std::string text;
    for (const auto& item : items) {
        if (!text.empty()) text += ' ';
        text += std::to_string(item.first) + ":" + Hex(static_cast<std::uint32_t>(item.second));
    }
    return text;
}

// Records are written one `name<TAB>value` line each, ended by a line `end`.
inline std::string Serialize(const Record& record) {
    std::string out;
    for (const auto& entry : record) out += entry.first + "\t" + entry.second + "\n";
    return out + "end\n";
}

}  // namespace nms_differential
