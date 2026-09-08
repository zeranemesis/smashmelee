#include "port/port_version.h"
#include "aurora/lib/logging.hpp"

#include <dvd.h>

namespace partyboard::version {

aurora::Module PartyBoardVersionLog("partyboard::version");

using namespace std::string_view_literals;

static bool versionInitialized;
static GameVersion gameVersion;
static DVDDiskID diskId;

void init() {
    versionInitialized = true;

    if (!DVDLowReadDiskID(&diskId, nullptr)) {
        PartyBoardVersionLog.fatal("DVDLowReadDiskID failed to return instantly.");
    }

    std::string_view company(diskId.company, sizeof(diskId.company));
    std::string_view game(diskId.gameName, sizeof(diskId.gameName));

    if (company != "01"sv) {
        PartyBoardVersionLog.fatal("Wrong company ID in disc: {}", company);
    }

    if (game == "GALE"sv && diskId.gameVersion == 2) {
        gameVersion = GameVersion::MeleeUsa102;
    } else {
        PartyBoardVersionLog.fatal(
            "Unknown/unsupported game version in disc: {}{} revision {}",
            game,
            company,
            diskId.gameVersion);
    }

    PartyBoardVersionLog.info("Loaded game disc is {}{}", game, company);
}

bool isRegionJpn() {
    return getGameVersion() == GameVersion::Jpn;
}

bool isRegionPal() {
    return getGameVersion() == GameVersion::PalRev0 || getGameVersion() == GameVersion::PalRev1 || getGameVersion() == GameVersion::PalRev2;
}

bool isRegionUsa() {
    return getGameVersion() == GameVersion::UsaRev0 || getGameVersion() == GameVersion::UsaRev1 ||
           getGameVersion() == GameVersion::MeleeUsa102;
}

GameVersion getGameVersion() {
    if (!versionInitialized) {
        abort();
    }

    return gameVersion;
}

const DVDDiskID& getDiskID() {
    return diskId;
}

}  // namespace partyboard::version

extern "C" u8 partyboard_version_get_version(void)
{
    return static_cast<u8>(partyboard::version::getGameVersion());
}

extern "C" BOOL partyboard_version_is_pal(void)
{
    return partyboard::version::isRegionPal();
}

extern "C" BOOL partyboard_version_is_usa(void)
{
    return partyboard::version::isRegionUsa();
}

extern "C" BOOL partyboard_version_is_ntsc(void)
{
    return partyboard::version::isRegionUsa() || partyboard::version::isRegionJpn();
}
