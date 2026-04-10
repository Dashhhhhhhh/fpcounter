#include <Geode/modify/GJBaseGameLayer.hpp>

#include "../FrameCheck.hpp"

using namespace geode::prelude;

class $modify(FrameCheckBaseGameLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        framecheck::beforeProcessCommands(this, dt, isHalfTick, isLastTick);
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        framecheck::afterProcessCommands(this, dt, isHalfTick, isLastTick);
    }
};
