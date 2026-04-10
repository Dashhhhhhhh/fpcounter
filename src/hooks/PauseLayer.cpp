#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/FLAlertLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/utils/general.hpp>

#include "../FrameCheck.hpp"

#ifdef _WIN32
    #include <commdlg.h>
    #include <cwchar>
    #include <windows.h>
    #pragma comment(lib, "Comdlg32.lib")
#endif

#include <array>
#include <chrono>
#include <optional>

using namespace geode::prelude;

namespace {
    std::chrono::steady_clock::time_point s_pickerSuppressedUntil {};

    class FrameCheckPauseTicker final : public cocos2d::CCNode {
    public:
        static FrameCheckPauseTicker* create() {
            auto* node = new FrameCheckPauseTicker();
            if (node && node->init()) {
                node->autorelease();
                node->scheduleUpdate();
                return node;
            }
            delete node;
            return nullptr;
        }

        void update(float) override {
            framecheck::tickAnalysisFromUI();
        }
    };

    bool copyText(std::string const& text, char const* successMessage, char const* failMessage) {
        if (!geode::utils::clipboard::write(text)) {
            Notification::create(failMessage, NotificationIcon::Warning)->show();
            return false;
        }

        Notification::create(successMessage, NotificationIcon::Success)->show();
        return true;
    }

    std::optional<std::filesystem::path> pickMacroFile() {
#ifdef _WIN32
        std::array<wchar_t, 32768> pathBuffer {};
        std::wstring initialDir;
        auto current = framecheck::currentMacroPath().wstring();
        if (!current.empty()) {
            auto currentPath = framecheck::currentMacroPath();
            if (currentPath.has_parent_path()) {
                initialDir = currentPath.parent_path().wstring();
            }
        }

        OPENFILENAMEW dialog {};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = GetActiveWindow();
        dialog.lpstrFile = pathBuffer.data();
        dialog.nMaxFile = static_cast<DWORD>(pathBuffer.size());
        dialog.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
        dialog.lpstrFilter = L"Geometry Dash Replay (*.gdr2;*.gdr)\0*.gdr2;*.gdr\0All Files (*.*)\0*.*\0";
        dialog.lpstrTitle = L"Select Geometry Dash Replay";
        dialog.nFilterIndex = 1;
        dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

        if (GetOpenFileNameW(&dialog)) {
            auto path = std::filesystem::path(pathBuffer.data());
            geode::log::info("[FrameCheck] native file picker selected '{}'", path.string());
            return path;
        }

        auto error = CommDlgExtendedError();
        if (error != 0) {
            geode::log::warn("[FrameCheck] native file picker failed error={}", error);
            Notification::create(fmt::format("Picker error {}", error), NotificationIcon::Error)->show();
        }
#else
        geode::log::warn("[FrameCheck] macro file picker is only implemented for Windows in this prototype");
#endif
        return std::nullopt;
    }
}

class $modify(FrameCheckPauseLayer, PauseLayer) {
    void copyFrameCheckLogs() {
        copyText(
            framecheck::recentDebugLogText(),
            "FrameCheck logs copied",
            "Could not copy FrameCheck logs"
        );
    }

    void onFrameCheckLogs(cocos2d::CCObject*) {
        auto text = framecheck::recentDebugLogText();
        auto* alert = FLAlertLayer::create(
            nullptr,
            "FrameCheck Logs",
            text,
            "OK",
            nullptr,
            420.f,
            true,
            300.f,
            0.35f
        );

        if (alert) {
            alert->show();
        }
    }

    void onCopyFrameCheckLogs(cocos2d::CCObject*) {
        copyFrameCheckLogs();
    }

    void onCopyFrameCheckAnalysis(cocos2d::CCObject*) {
        copyText(
            framecheck::recentAnalysisLogText(),
            "FrameCheck analysis copied",
            "Could not copy analysis"
        );
    }

    void onPickFrameCheckMacro(cocos2d::CCObject*) {
        auto now = std::chrono::steady_clock::now();
        if (now < s_pickerSuppressedUntil) {
            geode::log::warn("[FrameCheck] ignored duplicate macro picker activation");
            return;
        }
        s_pickerSuppressedUntil = now + std::chrono::seconds(3);

        Notification::create("Pick a GDR2 macro", NotificationIcon::Info)->show();

        auto selected = pickMacroFile();
        s_pickerSuppressedUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        if (!selected.has_value()) {
            Notification::create("GDR2 pick cancelled", NotificationIcon::Warning)->show();
            geode::log::info("[FrameCheck] file picker cancelled");
            return;
        }

        if (!framecheck::loadMacroFile(*selected)) {
            Notification::create("GDR2 load failed", NotificationIcon::Error)->show();
            return;
        }

        Notification::create("GDR2 loaded", NotificationIcon::Success)->show();
    }

    void onAnalyzeFrameCheck(cocos2d::CCObject*) {
        auto ok = framecheck::analyzeCurrentMacro();
        Notification::create(
            ok ? "FrameCheck analysis queued" : "FrameCheck analysis failed",
            ok ? NotificationIcon::Info : NotificationIcon::Error
        )->show();
    }

    void customSetup() {
        PauseLayer::customSetup();

        if (auto* ticker = FrameCheckPauseTicker::create()) {
            ticker->setID("framecheck-pause-analysis-ticker"_spr);
            this->addChild(ticker);
        }

        auto* menu = cocos2d::CCMenu::create();
        menu->setPosition(cocos2d::CCPointZero);
        menu->setID("framecheck-pause-menu"_spr);
        this->addChild(menu);

        auto* sprite = ButtonSprite::create("FC Logs", "goldFont.fnt", "GJ_button_04.png", 0.58f);
        sprite->setScale(0.72f);

        auto* button = CCMenuItemSpriteExtra::create(
            sprite,
            this,
            menu_selector(FrameCheckPauseLayer::onFrameCheckLogs)
        );
        button->setID("framecheck-logs-button"_spr);
        button->setPosition({48.f, 52.f});
        menu->addChild(button);

        auto* copySprite = ButtonSprite::create("Copy FC", "goldFont.fnt", "GJ_button_02.png", 0.58f);
        copySprite->setScale(0.72f);

        auto* copyButton = CCMenuItemSpriteExtra::create(
            copySprite,
            this,
            menu_selector(FrameCheckPauseLayer::onCopyFrameCheckLogs)
        );
        copyButton->setID("framecheck-copy-logs-button"_spr);
        copyButton->setPosition({48.f, 80.f});
        menu->addChild(copyButton);

        auto* copyAnalysisSprite = ButtonSprite::create("Copy Analysis", "goldFont.fnt", "GJ_button_02.png", 0.48f);
        copyAnalysisSprite->setScale(0.62f);

        auto* copyAnalysisButton = CCMenuItemSpriteExtra::create(
            copyAnalysisSprite,
            this,
            menu_selector(FrameCheckPauseLayer::onCopyFrameCheckAnalysis)
        );
        copyAnalysisButton->setID("framecheck-copy-analysis-button"_spr);
        copyAnalysisButton->setPosition({48.f, 108.f});
        menu->addChild(copyAnalysisButton);

        auto* pickSprite = ButtonSprite::create("Pick GDR2", "goldFont.fnt", "GJ_button_04.png", 0.56f);
        pickSprite->setScale(0.66f);

        auto* pickButton = CCMenuItemSpriteExtra::create(
            pickSprite,
            this,
            menu_selector(FrameCheckPauseLayer::onPickFrameCheckMacro)
        );
        pickButton->setID("framecheck-pick-macro-button"_spr);
        pickButton->setPosition({48.f, 136.f});
        menu->addChild(pickButton);

        auto* analyzeSprite = ButtonSprite::create("Analyze FC", "goldFont.fnt", "GJ_button_01.png", 0.58f);
        analyzeSprite->setScale(0.72f);

        auto* analyzeButton = CCMenuItemSpriteExtra::create(
            analyzeSprite,
            this,
            menu_selector(FrameCheckPauseLayer::onAnalyzeFrameCheck)
        );
        analyzeButton->setID("framecheck-analyze-button"_spr);
        analyzeButton->setPosition({48.f, 164.f});
        menu->addChild(analyzeButton);
    }
};
