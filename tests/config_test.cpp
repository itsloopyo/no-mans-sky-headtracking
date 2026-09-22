// Boundary validation for HeadTracking.ini.
//
// The file is seeded next to the game exe and edited by hand, so it is the one
// place raw user text becomes a float that reaches the camera basis, a port the
// receiver binds, or a virtual key the poller watches. Every case below is one
// character away in a real edit and none of them is survivable downstream:
//
//   UDPPort=abc      IniReader::ReadInt is the one reader that does NOT honour
//                    its default on a present-but-unparseable value - it hands
//                    back 0 - and a receiver bound to port 0 takes an ephemeral
//                    port no tracker is sending to.
//   UDPPort=70000    cast straight to uint16_t this truncates to 4464, so the
//                    mod listens on a port the user never named.
//   LimitZ=-0.40     inverts the bounds of math::Clamp(v, -limit, limit), which
//                    returns the lower bound for every input once they cross,
//                    so the camera parks at a fixed lean and looks jammed.
//   LimitX=nan       every comparison against NaN is false, so it skips the
//                    clamp entirely and writes a NaN camera basis every frame.
//   LocalSmoothing=0,15   a European decimal comma. strtod parses the prefix,
//                    yields 0.0, and that sits inside the valid range and
//                    passes every check with nothing in the log.
//   Enabled=true ; x GetPrivateProfileStringA does not strip inline comments and
//                    ReadBool matches the whole value, so this silently reverts
//                    the setting to its default.
//   ToggleKey=End    ReadHex is a strtol PREFIX parse: this reads as 0x0E, a
//                    code GetAsyncKeyState never reports, so the key does
//                    nothing and looks like a broken mod.

#include "core/config.h"

#include <cameraunlock/ads/ads_mode.h>

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (ok) return;
    ++g_failures;
    std::printf("FAIL: %s\n", what);
}

std::string TempIniPath() {
    char dir[MAX_PATH] = {};
    const DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::string(dir, n) + "nmsht_config_test.ini";
}

// GetPrivateProfileString caches per file, keyed on path and write time, so a
// suite that rewrites one path in a tight loop can be served a previous body.
// Deleting between writes is what makes each case read its own text.
bool WriteIni(const std::string& path, const std::string& body) {
    DeleteFileA(path.c_str());
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || f == nullptr) return false;
    const size_t written = std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);
    return written == body.size();
}

// Every case starts from the shipped defaults, so anything a case does not
// mention is asserted to have kept its default rather than to have been
// overwritten by the previous case.
bool Load(const std::string& path, const std::string& body, NMSHT::Config& out) {
    if (!WriteIni(path, body)) return false;
    out = NMSHT::Config{};
    return out.LoadFromIni(path);
}

void PortOutOfRangeKeepsTheDefault(const std::string& path) {
    NMSHT::Config c;
    Check(Load(path, "[Network]\nUDPPort=70000\n", c), "the ini loads");
    Check(c.udpPort == 4242, "a port past 65535 keeps 4242 instead of truncating to 4464");

    Check(Load(path, "[Network]\nUDPPort=abc\n", c), "the ini loads");
    Check(c.udpPort == 4242, "an unparseable port keeps 4242 instead of binding port 0");

    Check(Load(path, "[Network]\nUDPPort=80\n", c), "the ini loads");
    Check(c.udpPort == 4242, "a privileged port keeps 4242");

    Check(Load(path, "[Network]\nUDPPort=6002\n", c), "the ini loads");
    Check(c.udpPort == 6002, "a port inside 1024-65535 is taken as written");
}

void NegativePositionLimitsAreRefused(const std::string& path) {
    NMSHT::Config c;
    Check(Load(path,
               "[Position]\nLimitX=-0.30\nLimitY=-0.20\nLimitZ=-0.40\nLimitZBack=-0.10\n",
               c),
          "the ini loads");
    // The OUTCOME, not just the sign. A negative limit is clamped to the low
    // bound of its guard, which is 0, so the axis ends up disabled rather than
    // inverted - that is the deliberate choice config.cpp documents, and it is
    // what a user who mistypes a sign actually gets. Asserting only `>= 0` could
    // not tell that apart from "corrected back to the shipped default", so the
    // case passed whichever of the two the code did.
    Check(c.posLimitX == 0.0f && c.posLimitY == 0.0f && c.posLimitYDown == 0.0f &&
              c.posLimitZ == 0.0f && c.posLimitZBack == 0.0f,
          "a negative position limit clamps to zero, disabling that travel "
          "rather than inverting the clamp");
}

void NonFiniteValuesAreRefused(const std::string& path) {
    NMSHT::Config c;
    Check(Load(path,
               "[Sensitivity]\nYawMultiplier=nan\nPitchMultiplier=inf\n"
               "[Smoothing]\nLocalSmoothing=nan\nRemoteSmoothing=1e400\n"
               "[Position]\nLimitX=nan\n",
               c),
          "the ini loads");
    Check(std::isfinite(c.yawSensitivity) && std::isfinite(c.pitchSensitivity),
          "a non-finite sensitivity never reaches the camera basis");
    Check(std::isfinite(c.localSmoothing) && std::isfinite(c.remoteSmoothing),
          "a non-finite smoothing value never reaches exp()");
    Check(std::isfinite(c.posLimitX) && c.posLimitX >= 0.0f,
          "a non-finite position limit never reaches the clamp");
    Check(c.localSmoothing == 0.0f, "LocalSmoothing falls back to its own default, 0.0");
    Check(c.remoteSmoothing == 0.15f, "RemoteSmoothing falls back to its own default, 0.15");
}

void SmoothingRangeIsClampedNotFloored(const std::string& path) {
    NMSHT::Config c;
    Check(Load(path, "[Smoothing]\nLocalSmoothing=0.0\nRemoteSmoothing=5.0\n", c),
          "the ini loads");
    Check(c.localSmoothing == 0.0f, "a configured 0.0 stays 0.0 - validation, never a floor");
    Check(c.remoteSmoothing == 1.0f, "a smoothing value above 1 is clamped to 1");
}

void ADecimalCommaIsRejectedNotSilentlyRead(const std::string& path) {
    NMSHT::Config c;
    // RemoteSmoothing, not LocalSmoothing. A prefix strtod of "0,15" yields
    // 0.0, which is LocalSmoothing's own default, so that assertion held
    // whether or not the strict parse existed at all.
    Check(Load(path, "[Smoothing]\nRemoteSmoothing=0,15\n", c), "the ini loads");
    Check(c.remoteSmoothing == 0.15f,
          "a decimal comma falls back to the default rather than parsing as a prefix");

    Check(Load(path, "[Position]\nLimitZ=0,40\n", c), "the ini loads");
    Check(c.posLimitZ == 0.40f, "and the default it falls back to is the shipped one");
}

void InlineCommentsSurviveOnEveryType(const std::string& path) {
    NMSHT::Config c;
    Check(Load(path,
               "[General]\nAutoEnable=false ; off for now\n"
               "[Position]\nLimitZ=0.25 ; a shorter lean\n"
               "[ADS]\nMode=tracked ; keep looking around\n"
               "[Hotkeys]\nToggleKey=0x24 ; Home\n",
               c),
          "the ini loads");
    Check(c.autoEnable == false, "a bool with a trailing comment is still read");
    Check(c.posLimitZ == 0.25f, "a float with a trailing comment is still read");
    Check(c.adsMode == cameraunlock::ads::AdsMode::Tracked,
          "an ADS mode with a trailing comment is still read");
    Check(c.toggleKey == 0x24, "a hotkey with a trailing comment is still read");
}

void UnpollableHotkeysKeepTheDefault(const std::string& path) {
    NMSHT::Config c;
    Check(Load(path, "[Hotkeys]\nToggleKey=0x230\n", c), "the ini loads");
    Check(c.toggleKey == 0x23, "a key code past 0xFE keeps the End default");

    Check(Load(path, "[Hotkeys]\nToggleKey=End\n", c), "the ini loads");
    Check(c.toggleKey == 0x23, "a key NAME keeps the default instead of reading as 0x0E");

    Check(Load(path, "[Hotkeys]\nCycleModeKey=0x10\n", c), "the ini loads");
    Check(c.cycleModeKey == 0x21,
          "Shift keeps the Page Up default - the modifiers belong to the chord guard");

    Check(Load(path, "[Hotkeys]\nAdsModeKey=0x2E\n", c), "the ini loads");
    Check(c.adsModeKey == 0x2E, "a bindable key is taken as written");
}

void AnUnknownAdsModeLandsOnTheDefault(const std::string& path) {
    NMSHT::Config c;
    Check(Load(path, "[ADS]\nMode=off\n", c), "the ini loads");
    Check(c.adsMode == cameraunlock::ads::kDefaultAdsMode,
          "an unknown ADS mode lands on the default, not on whichever branch is last");
}

void AMissingFileIsReported(const std::string& path) {
    DeleteFileA(path.c_str());
    NMSHT::Config c;
    Check(!c.LoadFromIni(path), "a missing ini reports failure rather than half-loading");
    Check(c.udpPort == 4242 && c.posLimitZ == 0.40f, "and every value keeps its default");
}

void ARoundTripOfTheWrittenDefaultsReadsBackIdentical(const std::string& path) {
    DeleteFileA(path.c_str());

    // At least one value in every section is moved OFF its default first.
    // Writing the defaults and reading them into a default-constructed Config
    // compared default against default: a reader that parsed nothing at all -
    // a section lost to a BOM, a renamed key, a changed encoding - passed every
    // one of the comparisons below, which is the failure this case exists for.
    NMSHT::Config written;
    written.udpPort = 6002;
    written.yawSensitivity = 1.7f;
    written.positionEnabled = false;
    written.posLimitX = 0.22f;
    written.localSmoothing = 0.42f;
    written.remoteSmoothing = 0.60f;
    written.posLimitZ = 0.33f;
    written.posLimitZBack = 0.07f;
    written.posLimitYDown = 0.11f;
    written.toggleKey = 0x2E;
    written.cycleModeKey = 0x2D;
    // Not 0x24: VK_HOME is one of the two bindings the doctrine keeps
    // permanently free, and this case writes its value into a real ini.
    written.adsModeKey = 0x2F;
    written.adsMode = cameraunlock::ads::AdsMode::Tracked;
    written.autoEnable = false;
    written.diagnostics = true;
    written.reticleFollowsAim = false;
    Check(written.WriteDefault(path), "the seeded ini is written");

    NMSHT::Config read;
    Check(read.LoadFromIni(path), "and reads back");
    Check(read.udpPort == written.udpPort, "port survives the round trip");
    Check(read.yawSensitivity == written.yawSensitivity, "[Sensitivity] survives");
    Check(read.positionEnabled == written.positionEnabled, "[Position] Enabled survives");
    Check(read.posLimitX == written.posLimitX, "lateral limit survives");
    Check(read.localSmoothing == written.localSmoothing, "local smoothing survives");
    Check(read.remoteSmoothing == written.remoteSmoothing, "remote smoothing survives");
    Check(read.posLimitZ == written.posLimitZ, "forward lean limit survives");
    Check(read.posLimitZBack == written.posLimitZBack, "backward lean limit survives");
    Check(read.posLimitYDown == written.posLimitYDown, "downward lean limit survives");
    Check(read.toggleKey == written.toggleKey, "toggle key survives");
    Check(read.cycleModeKey == written.cycleModeKey, "cycle key survives");
    Check(read.adsModeKey == written.adsModeKey, "ADS key survives");
    Check(read.adsMode == written.adsMode, "ADS mode survives");
    Check(read.autoEnable == written.autoEnable, "AutoEnable survives");
    Check(read.diagnostics == written.diagnostics, "Diagnostics survives");
    Check(read.reticleFollowsAim == written.reticleFollowsAim,
          "[Reticle] FollowAim survives");
}

}  // namespace

int main() {
    const std::string path = TempIniPath();
    if (path.empty()) {
        std::printf("config_test: no temp directory\n");
        return EXIT_FAILURE;
    }
    std::printf("config_test: using %s\n", path.c_str());

    PortOutOfRangeKeepsTheDefault(path);
    NegativePositionLimitsAreRefused(path);
    NonFiniteValuesAreRefused(path);
    SmoothingRangeIsClampedNotFloored(path);
    ADecimalCommaIsRejectedNotSilentlyRead(path);
    InlineCommentsSurviveOnEveryType(path);
    UnpollableHotkeysKeepTheDefault(path);
    AnUnknownAdsModeLandsOnTheDefault(path);
    ARoundTripOfTheWrittenDefaultsReadsBackIdentical(path);
    AMissingFileIsReported(path);

    DeleteFileA(path.c_str());

    if (g_failures != 0) {
        std::printf("config_test: %d failure(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::printf("config_test: all cases passed\n");
    return EXIT_SUCCESS;
}
