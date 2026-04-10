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
        framecheck::onAttemptReset(this);
        PlayLayer::resetLevel();
    }

    void onExit() {
        framecheck::onLevelExit(this);
        PlayLayer::onExit();
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (framecheck::isSimulating()) {
            if (player) {
                player->m_isDead = true;
            }
            this->m_playerDied = true;
            return;
        }

        PlayLayer::destroyPlayer(player, object);
    }
};
