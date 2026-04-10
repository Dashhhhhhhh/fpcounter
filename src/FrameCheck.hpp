#pragma once

#include <filesystem>
#include <string>

class GJBaseGameLayer;
class PlayLayer;

namespace framecheck {
    void init();

    void onPlayLayerInit(PlayLayer* layer);
    void onLevelEnter(PlayLayer* layer);
    void onLevelExit(PlayLayer* layer);
    void onAttemptReset(PlayLayer* layer);

    bool loadMacroFile(std::filesystem::path const& path);
    std::filesystem::path currentMacroPath();

    void beforeProcessCommands(GJBaseGameLayer* layer, float dt, bool isHalfTick, bool isLastTick);
    void afterProcessCommands(GJBaseGameLayer* layer, float dt, bool isHalfTick, bool isLastTick);

    std::string recentDebugLogText();
    std::string recentFrameWindowText();
}
