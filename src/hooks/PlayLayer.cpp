#include <Geode/modify/PlayLayer.hpp>

#include "../FrameCheck.hpp"

using namespace geode::prelude;

class $modify(FrameCheckPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        framecheck::onPlayLayerInit(this);
        return true;
    }

    void onEnterTransitionDidFinish() {
        PlayLayer::onEnterTransitionDidFinish();
        framecheck::onLevelEnter(this);
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        framecheck::onAttemptReset(this);
    }

    void onExit() {
        framecheck::onLevelExit(this);
        PlayLayer::onExit();
    }
};
