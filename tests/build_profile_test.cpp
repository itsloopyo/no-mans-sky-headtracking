// The third-person commit site is a per-image RVA, and an RVA on the wrong
// profile is silent: the build it names never matches, so the site is simply
// never armed and the view keeps following your head in first person only.
// That shipped once, with the Steam address sitting on a GDK profile.
#include "../src/camera/build_profile.cpp"

#include <cstdio>

namespace {
int failures = 0;

void Check(bool passed, const char* label) {
    if (!passed) {
        std::printf("FAIL: %s\n", label);
        ++failures;
    }
}

const NMSHT::BuildProfile* ProfileNamed(const char* id) {
    for (const NMSHT::BuildProfile* p : NMSHT::kKnownProfiles) {
        if (std::strcmp(p->name, id) == 0) return p;
    }
    return nullptr;
}
}  // namespace

int main() {
    const auto* steam = ProfileNamed("steam-win64-20260921");
    Check(steam != nullptr, "the September 21 Steam profile is still in the table");
    if (steam != nullptr) {
        Check(steam->cameraCommitRva == 0x00651FD8u,
              "first-person commit site unchanged");
        Check(steam->cameraCommitThirdPersonRva == 0x0067FBC6u,
              "third-person commit site is pinned on the image it was read from");
    }

    // The GDK builds route every behaviour through one shared writer, which
    // cameraCommitRva already pins. A second site there would arm an address
    // that means nothing in that image.
    for (const NMSHT::BuildProfile* p : NMSHT::kKnownProfiles) {
        if (std::strncmp(p->name, "gdk-", 4) != 0) continue;
        Check(p->cameraCommitThirdPersonRva == 0,
              "a GDK profile pins no separate third-person site");
    }

    // Two sites in one profile must be two addresses, or the second arms a
    // debug register on the first site and the handler reports it twice.
    for (const NMSHT::BuildProfile* p : NMSHT::kKnownProfiles) {
        if (p->cameraCommitThirdPersonRva == 0) continue;
        Check(p->cameraCommitThirdPersonRva != p->cameraCommitRva,
              "the third-person site differs from the first-person one");
    }

    if (failures == 0) std::printf("build_profile_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
