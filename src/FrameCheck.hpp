#pragma once

#include <Geode/Geode.hpp>

#include <filesystem>

class GJBaseGameLayer;
class PlayLayer;

namespace framecheck {
    void init();

    void onPlayLayerInit(PlayLayer* layer);
    void onLevelEnter(PlayLayer* layer);
    void onLevelExit(PlayLayer* layer);
    void onAttemptReset(PlayLayer* layer);
    bool loadMacroFile(std::filesystem::path const& path);
    bool analyzeCurrentMacro();
    std::filesystem::path currentMacroPath();

    void afterProcessCommands(GJBaseGameLayer* layer, float dt, bool isHalfTick, bool isLastTick);
    void beforeHandleButton(GJBaseGameLayer* layer, bool down, int button, bool isPlayer1);
    void tickAnalysisFromUI();

    std::string recentDebugLogText();
    std::string recentAnalysisLogText();
    std::string recentFrameWindowText();
    bool isSimulating();
}
