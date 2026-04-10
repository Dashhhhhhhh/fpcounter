#include <Geode/modify/GJBaseGameLayer.hpp>

#include "../FrameCheck.hpp"

using namespace geode::prelude;

class $modify(FrameCheckBaseGameLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        framecheck::afterProcessCommands(this, dt, isHalfTick, isLastTick);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        framecheck::beforeHandleButton(this, down, button, isPlayer1);
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};
