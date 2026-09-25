// HeadTracking.ini on the canonical format: the committed file is what the
// table renders, a first launch creates exactly those bytes, and the tracking
// mode cycle changes the lines of its own rows and no other byte.
//
// config_test --render-config <path> writes the rendered file to <path>
// instead (pixi run render-config).

#include "core/config.h"

#include <cameraunlock/config/canonical_ini.h>

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace cfg = cameraunlock::config;

using NMSHT::Config;

namespace {

int g_failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        ++g_failures;
    }
}

std::string ReadBytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path.string());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteBytes(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("cannot write " + path.string());
}

std::string Rendered() {
    const cfg::ConfigTable<Config> table = NMSHT::ConfigTable();
    cfg::RenderHeader header;
    header.display_name = NMSHT::kGameDisplayName;
    return cfg::RenderCanonical(table, table.defaults(), header);
}

std::vector<std::string> Lines(const std::string& bytes) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start < bytes.size()) {
        const std::size_t end = bytes.find("\r\n", start);
        if (end == std::string::npos) throw std::logic_error("a rendered line does not end in CRLF");
        lines.push_back(bytes.substr(start, end - start));
        start = end + 2;
    }
    return lines;
}

// The lines that differ between two files with the same number of lines.
std::vector<std::string> ChangedLines(const std::string& before, const std::string& after) {
    const std::vector<std::string> a = Lines(before);
    const std::vector<std::string> b = Lines(after);
    if (a.size() != b.size()) throw std::logic_error("a save added or removed a line");
    std::vector<std::string> changed;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] != b[i]) changed.push_back(a[i] + " -> " + b[i]);
    }
    return changed;
}

fs::path TempDir() {
    wchar_t temp[MAX_PATH + 1];
    if (GetTempPathW(MAX_PATH + 1, temp) == 0) throw std::runtime_error("GetTempPathW failed");
    const fs::path dir = fs::path(temp) / ("nms-config-tests-" + std::to_string(GetCurrentProcessId()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void RenderMatchesCommittedFile() {
    const std::string committed = ReadBytes(fs::path(NMS_SOURCE_DIR) / "HeadTracking.ini");
    Check(committed == Rendered(), "HeadTracking.ini is not what the table renders; run pixi run render-config");
    const cfg::CanonicalIni doc = cfg::ParseCanonicalIni(committed);
    Check(doc.IsReadable() && doc.diagnostics.empty(), "the committed file reads without a diagnostic");
    Config read = NMSHT::ConfigTable().defaults();
    Check(cfg::ApplyCanonical(doc, NMSHT::ConfigTable(), read).diagnostics.empty(),
          "the committed file applies without a diagnostic");
}

void DefaultsAreTheFleetDefaults() {
    using cameraunlock::input::KeyModifiers;
    const Config defaults = NMSHT::ConfigTable().defaults();
    Check(defaults.toggleKey == "End, Ctrl+Shift+Y", "ToggleKey defaults to End, Ctrl+Shift+Y");
    Check(defaults.cycleTrackingModeKey == "PageUp, Ctrl+Shift+G",
          "CycleTrackingModeKey defaults to PageUp, Ctrl+Shift+G");
    Check(defaults.enableOnStartup, "head tracking is on at startup by default");
    Check(NMSHT::StartupTrackingMode(defaults) == cameraunlock::TrackingMode::RotationAndPosition,
          "the default tracking mode is rotation and position");
    const auto toggle = NMSHT::KeyBindings(defaults.toggleKey);
    Check(toggle.size() == 2 && toggle[0].vk == VK_END && toggle[0].modifiers == KeyModifiers::kNone &&
              toggle[1].vk == 'Y' && toggle[1].modifiers == (KeyModifiers::kCtrl | KeyModifiers::kShift),
          "ToggleKey registers End and Ctrl+Shift+Y");
    const auto cycle = NMSHT::KeyBindings(defaults.cycleTrackingModeKey);
    Check(cycle.size() == 2 && cycle[0].vk == VK_PRIOR && cycle[0].modifiers == KeyModifiers::kNone &&
              cycle[1].vk == 'G' && cycle[1].modifiers == (KeyModifiers::kCtrl | KeyModifiers::kShift),
          "CycleTrackingModeKey registers PageUp and Ctrl+Shift+G");
    const auto freeze = NMSHT::KeyBindings(defaults.sweepFreezeKey);
    Check(freeze.size() == 1 && freeze[0].vk == 'J' &&
              freeze[0].modifiers == (KeyModifiers::kCtrl | KeyModifiers::kShift),
          "SweepFreezeKey registers Ctrl+Shift+J");
    Check(defaults.aimTransformCallers.empty() && defaults.aimCopyCallers.empty() &&
              defaults.trackedTransformCallers.empty() && defaults.sceneSampleRva.empty(),
          "the [Debug] address lists default to the build's own");
}

void TheModeCycleSavesOnlyItsRows(const fs::path& dir) {
    const fs::path path = dir / "HeadTracking.ini";
    cfg::ConfigOwner<Config> owner(NMSHT::ConfigOwnerOptions(path.wstring()));
    Check(owner.Load().status == cfg::ConfigLoadStatus::Created, "a first launch creates the file");
    const std::string fresh = ReadBytes(path);
    Check(fresh == Rendered(), "a first launch writes the committed file's bytes");

    const auto save = [&owner](cameraunlock::TrackingMode mode) {
        const auto channels = cameraunlock::EncodeTrackingMode(mode);
        return owner.Save([channels](Config& c) {
            c.rotationEnabled = channels.rotation_enabled;
            c.positionEnabled = channels.position_enabled;
        });
    };

    Check(save(cameraunlock::TrackingMode::RotationOnly).status == cfg::ConfigSaveStatus::Saved,
          "rotation only saves");
    const std::string rotationOnly = ReadBytes(path);
    const auto first = ChangedLines(fresh, rotationOnly);
    Check(first.size() == 1 && first[0] == "PositionEnabled=true -> PositionEnabled=false",
          "saving rotation only changes the PositionEnabled line and no other byte");

    Check(save(cameraunlock::TrackingMode::PositionOnly).status == cfg::ConfigSaveStatus::Saved,
          "position only saves");
    const auto second = ChangedLines(rotationOnly, ReadBytes(path));
    Check(second.size() == 2 && second[0] == "RotationEnabled=true -> RotationEnabled=false" &&
              second[1] == "PositionEnabled=false -> PositionEnabled=true",
          "saving position only changes the two mode lines and no other byte");

    bool refused = false;
    try {
        owner.Save([](Config& c) { c.enableOnStartup = false; });
    } catch (const std::logic_error&) {
        refused = true;
    }
    Check(refused, "EnableOnStartup is not Writable, so the toggle key never saves it");

    Check(owner.Reload().status == cfg::ConfigReloadStatus::Unchanged, "a reload after a save finds nothing new");

    cfg::ConfigOwner<Config> relaunch(NMSHT::ConfigOwnerOptions(path.wstring()));
    const auto again = relaunch.Load();
    Check(again.status == cfg::ConfigLoadStatus::Canonical, "the next launch reads the saved file as canonical");
    Check(NMSHT::StartupTrackingMode(again.config) == cameraunlock::TrackingMode::PositionOnly,
          "the saved tracking mode comes back");
    Check(again.config.enableOnStartup, "EnableOnStartup stays as the file had it");
}

void AnEditIsPickedUpMidSession(const fs::path& dir) {
    const fs::path path = dir / "edited" / "HeadTracking.ini";
    fs::create_directories(path.parent_path());
    cfg::ConfigOwner<Config> owner(NMSHT::ConfigOwnerOptions(path.wstring()));
    Check(owner.Load().status == cfg::ConfigLoadStatus::Created, "a first launch creates the file");
    std::string edited = ReadBytes(path);
    const std::string from = "; AimCopyCallers=\r\n";
    const std::size_t at = edited.find(from);
    Check(at != std::string::npos, "an unset caller list is written as a comment");
    if (at == std::string::npos) return;
    edited.replace(at, from.size(), "AimCopyCallers=0x1A2B, 0x3C4D\r\n");
    Sleep(20);
    WriteBytes(path, edited);
    Check(owner.FileChanged(), "the owner sees the edit");
    const auto reloaded = owner.Reload();
    Check(reloaded.status == cfg::ConfigReloadStatus::Applied && reloaded.config &&
              reloaded.config->aimCopyCallers == std::vector<std::uint32_t>{0x1A2B, 0x3C4D},
          "a reload reads the edited caller list");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--render-config") {
        WriteBytes(argv[2], Rendered());
        return 0;
    }
    if (argc != 1) {
        std::fprintf(stderr, "usage: config_test [--render-config <path>]\n");
        return 2;
    }

    const fs::path dir = TempDir();
    RenderMatchesCommittedFile();
    DefaultsAreTheFleetDefaults();
    TheModeCycleSavesOnlyItsRows(dir);
    AnEditIsPickedUpMidSession(dir);
    fs::remove_all(dir);

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("Config tests passed");
    return 0;
}
