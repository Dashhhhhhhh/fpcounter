#include "FrameCheck.hpp"

#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/PlayLayer.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace framecheck {
namespace {
    constexpr char const* kDefaultMacroPath = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Geometry Dash\\replays\\acu.gdr2";
    constexpr double kFallbackFramerate = 240.0;
    constexpr size_t kMaxDebugLines = 200;
    struct MacroInput {
        uint64_t frame = 0;
        uint8_t button = static_cast<uint8_t>(PlayerButton::Jump);
        bool down = false;
        size_t ordinal = 0;
    };

    struct MacroReplay {
        bool loaded = false;
        int version = 0;
        std::string author;
        std::string description;
        double framerate = kFallbackFramerate;
        bool platformer = false;
        std::string levelName;
        uint32_t levelID = 0;
        std::vector<MacroInput> inputs;
        std::string error;
    };

    class BinaryReader {
    public:
        explicit BinaryReader(std::vector<uint8_t> data) : m_data(std::move(data)) {}

        size_t remaining() const {
            return m_pos <= m_data.size() ? m_data.size() - m_pos : 0;
        }

        bool readBytes(uint8_t* out, size_t count) {
            if (remaining() < count) return false;
            std::memcpy(out, m_data.data() + m_pos, count);
            m_pos += count;
            return true;
        }

        std::optional<uint64_t> readVarUInt() {
            uint64_t value = 0;
            int shift = 0;
            for (int i = 0; i < 10; ++i) {
                if (remaining() == 0) return std::nullopt;
                auto byte = m_data[m_pos++];
                value |= static_cast<uint64_t>(byte & 0x7f) << shift;
                if ((byte & 0x80) == 0) return value;
                shift += 7;
            }
            return std::nullopt;
        }

        bool readString(std::string& out) {
            auto length = readVarUInt();
            if (!length.has_value() || *length > 0xffff || remaining() < *length) return false;
            out.assign(reinterpret_cast<char const*>(m_data.data() + m_pos), static_cast<size_t>(*length));
            m_pos += static_cast<size_t>(*length);
            return true;
        }

        bool readBool(bool& out) {
            if (remaining() < 1) return false;
            out = m_data[m_pos++] != 0;
            return true;
        }

        bool readFloat(float& out) {
            uint8_t bytes[4] {};
            if (!readBytes(bytes, sizeof(bytes))) return false;
            uint32_t packed =
                (static_cast<uint32_t>(bytes[0]) << 24) |
                (static_cast<uint32_t>(bytes[1]) << 16) |
                (static_cast<uint32_t>(bytes[2]) << 8) |
                static_cast<uint32_t>(bytes[3]);
            std::memcpy(&out, &packed, sizeof(out));
            return true;
        }

        bool readDouble(double& out) {
            uint8_t bytes[8] {};
            if (!readBytes(bytes, sizeof(bytes))) return false;
            uint64_t packed = 0;
            for (auto byte : bytes) packed = (packed << 8) | byte;
            std::memcpy(&out, &packed, sizeof(out));
            return true;
        }

        bool skip(size_t count) {
            if (remaining() < count) return false;
            m_pos += count;
            return true;
        }

    private:
        std::vector<uint8_t> m_data;
        size_t m_pos = 0;
    };

    PlayLayer* s_layer = nullptr;
    cocos2d::CCLabelBMFont* s_label = nullptr;
    MacroReplay s_macro;
    std::filesystem::path s_macroPath = kDefaultMacroPath;
    bool s_macroPlaybackEnabled = false;
    size_t s_macroPlaybackIndex = 0;
    uint64_t s_liveTick = 0;
    uint64_t s_liveAttemptBaseTick = 0;
    int s_liveAttemptBaseCommandIndex = 0;
    uint64_t s_lastPlaybackFrame = 0;
    uint64_t s_lastObservedCommandFrame = 0;
    uint64_t s_lastObservedCommandStride = 1;
    bool s_haveObservedCommandFrame = false;
    std::deque<std::string> s_debugLines;
    std::string s_lastLabelText = "Pick GDR2";

    void pushDebug(std::string const& line) {
        s_debugLines.push_back(line);
        while (s_debugLines.size() > kMaxDebugLines) s_debugLines.pop_front();
        log::info("{}", line);
    }

    template <class... Args>
    void fcLog(fmt::format_string<Args...> fmtString, Args&&... args) {
        pushDebug(fmt::format(std::move(fmtString), std::forward<Args>(args)...));
    }

    void setLabelText(std::string const& text) {
        s_lastLabelText = text;
        if (s_label) {
            s_label->setString(text.c_str());
            s_label->limitLabelWidth(240.f, 0.32f, 0.16f);
        }
    }

    std::string macroStatusText(std::string const& detail = "") {
        if (!s_macro.loaded) return detail.empty() ? "Pick GDR2" : detail;

        std::ostringstream out;
        out << "GDR2 " << (s_macroPlaybackEnabled ? "enabled" : "loaded");
        out << "\n" << s_macroPath.filename().string();
        out << "\n" << s_macroPlaybackIndex << "/" << s_macro.inputs.size() << " inputs";
        if (s_macroPlaybackIndex < s_macro.inputs.size()) {
            auto const& next = s_macro.inputs[s_macroPlaybackIndex];
            auto seconds = s_macro.framerate > 1.0
                ? static_cast<double>(next.frame) / s_macro.framerate
                : 0.0;
            out << fmt::format("\nnext #{} f{} ({:.3f}s)", next.ordinal, next.frame, seconds);
        }
        if (!detail.empty()) out << "\n" << detail;
        return out.str();
    }

    void createLabel(PlayLayer* layer) {
        if (!layer) return;

        auto size = cocos2d::CCDirector::sharedDirector()->getWinSize();
        s_label = cocos2d::CCLabelBMFont::create(s_lastLabelText.c_str(), "bigFont.fnt");
        if (!s_label) return;

        s_label->setAnchorPoint({0.f, 1.f});
        s_label->setPosition({6.f, size.height - 6.f});
        s_label->setScale(0.32f);
        s_label->limitLabelWidth(240.f, 0.32f, 0.16f);
        s_label->setColor({255, 80, 80});
        s_label->setZOrder(9999);
        s_label->setID("framecheck-debug-label"_spr);
        layer->addChild(s_label);
    }

    uint64_t currentLiveAttemptTick() {
        return s_liveTick >= s_liveAttemptBaseTick ? s_liveTick - s_liveAttemptBaseTick : 0;
    }

    PlayLayer* currentPlayLayer() {
        if (s_layer) return s_layer;
        auto* manager = GameManager::get();
        auto* layer = manager ? manager->m_playLayer : nullptr;
        if (layer) s_layer = layer;
        return layer;
    }

    uint64_t estimateReplayFrame(PlayLayer* layer) {
        if (!layer || !s_macro.loaded) return currentLiveAttemptTick();
        auto fps = s_macro.framerate > 1.0 ? s_macro.framerate : kFallbackFramerate;
        auto frame = static_cast<int64_t>(std::llround(layer->m_gameState.m_levelTime * fps));
        return frame > 0 ? static_cast<uint64_t>(frame) : 0;
    }

    uint64_t commandFrame(PlayLayer* layer) {
        if (!layer) return currentLiveAttemptTick();
        auto current = layer->m_gameState.m_commandIndex;
        return current >= s_liveAttemptBaseCommandIndex
            ? static_cast<uint64_t>(current - s_liveAttemptBaseCommandIndex)
            : 0;
    }

    uint64_t scaledTickFrame() {
        auto fps = s_macro.framerate > 1.0 ? s_macro.framerate : kFallbackFramerate;
        return static_cast<uint64_t>(std::llround(static_cast<double>(currentLiveAttemptTick()) * fps / 240.0));
    }

    std::optional<MacroReplay> parseGdr2File(std::filesystem::path const& path, std::string& error) {
        fcLog("[MacroParser] macro load path={}", path.string());

        std::ifstream file(path, std::ios::binary);
        if (!file) {
            error = "failed to open macro file";
            return std::nullopt;
        }

        file.seekg(0, std::ios::end);
        auto size = static_cast<size_t>(file.tellg());
        file.seekg(0, std::ios::beg);

        std::vector<uint8_t> bytes(size);
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            error = "failed to read macro file";
            return std::nullopt;
        }

        BinaryReader reader(std::move(bytes));

        uint8_t magic[3] {};
        if (!reader.readBytes(magic, sizeof(magic)) || std::string_view(reinterpret_cast<char*>(magic), 3) != "GDR") {
            error = "invalid GDR magic";
            return std::nullopt;
        }

        MacroReplay replay;
        auto version = reader.readVarUInt();
        if (!version.has_value()) {
            error = "missing GDR version";
            return std::nullopt;
        }
        replay.version = static_cast<int>(*version);

        std::string inputTag;
        float duration = 0.f;
        int gameVersion = 0;
        int seed = 0;
        int coins = 0;
        bool ldm = false;
        std::string botName;
        int botVersion = 0;

        auto readVarIntField = [&](int& out) -> bool {
            auto value = reader.readVarUInt();
            if (!value.has_value()) return false;
            out = static_cast<int>(*value);
            return true;
        };

        if (
            !reader.readString(inputTag) ||
            !reader.readString(replay.author) ||
            !reader.readString(replay.description) ||
            !reader.readFloat(duration) ||
            !readVarIntField(gameVersion) ||
            !reader.readDouble(replay.framerate) ||
            !readVarIntField(seed) ||
            !readVarIntField(coins) ||
            !reader.readBool(ldm) ||
            !reader.readBool(replay.platformer) ||
            !reader.readString(botName) ||
            !readVarIntField(botVersion)
        ) {
            error = "failed while reading replay metadata";
            return std::nullopt;
        }

        auto levelID = reader.readVarUInt();
        if (!levelID.has_value() || !reader.readString(replay.levelName)) {
            error = "failed while reading level metadata";
            return std::nullopt;
        }
        replay.levelID = static_cast<uint32_t>(*levelID);

        auto extensionSize = reader.readVarUInt();
        if (!extensionSize.has_value() || !reader.skip(static_cast<size_t>(*extensionSize))) {
            error = "invalid replay extension block";
            return std::nullopt;
        }

        auto deathCount = reader.readVarUInt();
        if (!deathCount.has_value()) {
            error = "missing death count";
            return std::nullopt;
        }
        for (uint64_t i = 0; i < *deathCount; ++i) {
            auto delta = reader.readVarUInt();
            if (!delta.has_value()) {
                error = "truncated death list";
                return std::nullopt;
            }
        }

        auto inputCount = reader.readVarUInt();
        auto p1InputCount = reader.readVarUInt();
        if (!inputCount.has_value() || !p1InputCount.has_value()) {
            error = "missing input counts";
            return std::nullopt;
        }

        uint64_t runningFrame = 0;
        uint64_t p1Remaining = *p1InputCount;
        auto hasInputExtension = !inputTag.empty();

        replay.inputs.reserve(static_cast<size_t>(*inputCount));
        while (reader.remaining() > 0 && replay.inputs.size() < *inputCount) {
            auto packed = reader.readVarUInt();
            if (!packed.has_value()) {
                error = "truncated input stream";
                return std::nullopt;
            }

            MacroInput input;
            if (replay.platformer) {
                auto delta = *packed >> 3;
                input.button = static_cast<uint8_t>((*packed >> 1) & 3);
                input.down = (*packed & 1) != 0;
                input.frame = runningFrame + delta;
            }
            else {
                auto delta = *packed >> 1;
                input.button = static_cast<uint8_t>(PlayerButton::Jump);
                input.down = (*packed & 1) != 0;
                input.frame = runningFrame + delta;
            }

            if (hasInputExtension) {
                auto inputExtensionSize = reader.readVarUInt();
                if (!inputExtensionSize.has_value() || !reader.skip(static_cast<size_t>(*inputExtensionSize))) {
                    error = "invalid input extension block";
                    return std::nullopt;
                }
            }

            input.ordinal = replay.inputs.size() + 1;
            replay.inputs.push_back(input);
            runningFrame = input.frame;

            if (p1Remaining > 0) {
                --p1Remaining;
                if (p1Remaining == 0) runningFrame = 0;
            }
        }

        std::stable_sort(replay.inputs.begin(), replay.inputs.end(), [](MacroInput const& a, MacroInput const& b) {
            if (a.frame != b.frame) return a.frame < b.frame;
            return a.ordinal < b.ordinal;
        });

        replay.loaded = true;
        fcLog(
            "[MacroParser] loaded level='{}' levelID={} fps={:.6f} platformer={} inputs={}",
            replay.levelName,
            replay.levelID,
            replay.framerate,
            replay.platformer ? "true" : "false",
            replay.inputs.size()
        );
        return replay;
    }

    void resetPlayback(char const* reason) {
        auto previousFrame = s_lastPlaybackFrame;
        s_macroPlaybackIndex = 0;
        s_liveAttemptBaseTick = s_liveTick;
        auto* layer = currentPlayLayer();
        s_liveAttemptBaseCommandIndex = layer ? layer->m_gameState.m_commandIndex : 0;
        s_lastPlaybackFrame = 0;
        s_lastObservedCommandFrame = 0;
        s_lastObservedCommandStride = 1;
        s_haveObservedCommandFrame = false;
        fcLog(
            "[FrameCheck/State] playback reset reason={} inputs={} previousFrame={} baseTick={} baseCommandIndex={}",
            reason,
            s_macro.loaded ? s_macro.inputs.size() : 0,
            previousFrame,
            s_liveAttemptBaseTick,
            s_liveAttemptBaseCommandIndex
        );
    }

    void pumpMacroPlayback(PlayLayer* layer) {
        if (!s_macroPlaybackEnabled || !s_macro.loaded || !layer) return;

        auto timelineFrame = commandFrame(layer);
        auto estimatedFrame = estimateReplayFrame(layer);
        auto tickFrame = currentLiveAttemptTick();
        auto scaledFrame = scaledTickFrame();
        if (s_haveObservedCommandFrame && timelineFrame > s_lastObservedCommandFrame) {
            s_lastObservedCommandStride = timelineFrame - s_lastObservedCommandFrame;
        }
        auto commandDispatchFrame = timelineFrame + s_lastObservedCommandStride;
        auto frame = timelineFrame;
        auto dispatchFrame = commandDispatchFrame;

        if (frame != s_lastPlaybackFrame && (frame < 8 || frame % 60 == 0)) {
            fcLog(
                "[FrameCheck/Sync] playback tick frame={} dispatchFrame={} commandFrame={} commandStride={} estFrame={} scaledTickFrame={} tickFrame={} commandIndex={} levelTime={:.6f}",
                frame,
                dispatchFrame,
                timelineFrame,
                s_lastObservedCommandStride,
                estimatedFrame,
                scaledFrame,
                tickFrame,
                layer->m_gameState.m_commandIndex,
                layer->m_gameState.m_levelTime
            );
        }
        s_lastPlaybackFrame = frame;

        while (s_macroPlaybackIndex < s_macro.inputs.size() && s_macro.inputs[s_macroPlaybackIndex].frame <= dispatchFrame) {
            auto const& input = s_macro.inputs[s_macroPlaybackIndex];
            fcLog(
                "[FrameCheck] replay input#{} frame={} liveFrame={} dispatchFrame={} commandFrame={} commandStride={} estFrame={} scaledTickFrame={} tickFrame={} down={} button={}",
                input.ordinal,
                input.frame,
                frame,
                dispatchFrame,
                timelineFrame,
                s_lastObservedCommandStride,
                estimatedFrame,
                scaledFrame,
                tickFrame,
                input.down ? "true" : "false",
                input.button
            );
            layer->handleButton(input.down, input.button, true);
            ++s_macroPlaybackIndex;
        }

        s_lastObservedCommandFrame = timelineFrame;
        s_haveObservedCommandFrame = true;

        if (s_macroPlaybackIndex >= s_macro.inputs.size()) {
            setLabelText(macroStatusText("macro complete"));
        }
        else {
            auto const& next = s_macro.inputs[s_macroPlaybackIndex];
            setLabelText(macroStatusText(fmt::format(
                "cmd {}->{} stride {} est {} st {}",
                timelineFrame,
                dispatchFrame,
                s_lastObservedCommandStride,
                estimatedFrame,
                scaledFrame
            )));
        }
    }
}

void init() {
    fcLog("[FrameCheck] init macroPath='{}'", s_macroPath.string());
}

void onPlayLayerInit(PlayLayer* layer) {
    s_layer = layer;
    s_label = nullptr;
    s_liveAttemptBaseCommandIndex = layer ? layer->m_gameState.m_commandIndex : 0;
    createLabel(layer);
    setLabelText(s_macro.loaded ? macroStatusText(s_macroPlaybackEnabled ? "ready" : "Pick GDR2") : "Pick GDR2");
}

void onLevelEnter(PlayLayer* layer) {
    if (layer != s_layer) return;
    s_liveAttemptBaseCommandIndex = layer->m_gameState.m_commandIndex;
    if (s_macroPlaybackEnabled) {
        resetPlayback("level-enter");
        setLabelText(macroStatusText("run level"));
    }
}

void onLevelExit(PlayLayer* layer) {
    if (layer != s_layer) return;
    s_layer = nullptr;
    s_label = nullptr;
}

void onAttemptReset(PlayLayer* layer) {
    if (layer != s_layer) return;
    if (s_macroPlaybackEnabled) {
        resetPlayback("attempt-reset");
        setLabelText(macroStatusText("attempt reset"));
    }
}

bool loadMacroFile(std::filesystem::path const& path) {
    s_macroPath = path;

    std::string error;
    auto replay = parseGdr2File(path, error);
    if (!replay.has_value()) {
        s_macro = {};
        s_macro.error = error;
        s_macroPlaybackEnabled = false;
        setLabelText("GDR2 load failed");
        fcLog("[MacroParser] load failed reason={}", error);
        return false;
    }

    s_macro = std::move(*replay);
    if (s_macro.platformer) {
        s_macroPlaybackEnabled = false;
        setLabelText("GDR2 platformer unsupported");
        fcLog("[FrameCheck] macro rejected reason=platformer");
        return false;
    }

    s_macroPlaybackEnabled = true;
    s_layer = currentPlayLayer();
    resetPlayback("macro-picked");
    setLabelText(macroStatusText("run level"));
    fcLog("[FrameCheck] macro enabled path='{}'", s_macroPath.string());
    return true;
}

std::filesystem::path currentMacroPath() {
    return s_macroPath;
}

void beforeProcessCommands(GJBaseGameLayer* layer, float, bool, bool) {
    auto* playLayer = static_cast<PlayLayer*>(layer);
    auto* active = currentPlayLayer();
    if (!playLayer || !active || playLayer != active) return;
    s_layer = active;
    ++s_liveTick;
    pumpMacroPlayback(playLayer);
}

void afterProcessCommands(GJBaseGameLayer*, float, bool, bool) {}

std::string recentDebugLogText() {
    std::ostringstream out;
    out << s_lastLabelText << "\n\n";
    if (!s_macro.loaded) {
        out << "Macro not loaded\n";
    }
    else {
        out << "Macro: " << s_macroPath.string() << "\n";
        out << "Level: " << s_macro.levelName << " (" << s_macro.levelID << ")\n";
        out << "FPS: " << s_macro.framerate << "\n";
        out << "Inputs: " << s_macro.inputs.size() << "\n";
        out << "Playback enabled: " << (s_macroPlaybackEnabled ? "true" : "false") << "\n";
        out << "Playback index: " << s_macroPlaybackIndex << "\n\n";
    }

    for (auto const& line : s_debugLines) out << line << "\n";
    return out.str();
}

std::string recentFrameWindowText() {
    return s_lastLabelText;
}
}
