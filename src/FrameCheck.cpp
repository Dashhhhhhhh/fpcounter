#include "FrameCheck.hpp"

#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/loader/Mod.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace framecheck {
namespace {
    constexpr char const* kDefaultMacroPath = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Geometry Dash\\replays\\acu.gdr2";
    constexpr int kProbeRadius = 6;
    constexpr int kMaxDisplayWindow = 5;
    constexpr int kPostInputTicks = 8;
    constexpr double kFallbackFramerate = 240.0;
    constexpr size_t kMaxDebugLines = 900;
    constexpr size_t kMaxLabelRows = 10;
    constexpr size_t kBranchStepsPerTick = 90;
    constexpr char const* kCacheKey = "macro-analysis-cache-v3";

    struct MacroInput {
        uint64_t frame = 0;
        uint8_t button = 1;
        bool player2 = false;
        bool down = false;
        size_t ordinal = 0;
    };

    struct MacroReplay {
        bool loaded = false;
        int version = 0;
        std::string inputTag;
        std::string author;
        std::string description;
        float duration = 0.f;
        int gameVersion = 0;
        double framerate = kFallbackFramerate;
        int seed = 0;
        int coins = 0;
        bool ldm = false;
        bool platformer = false;
        std::string botName;
        int botVersion = 0;
        uint32_t levelID = 0;
        std::string levelName;
        std::vector<uint64_t> deaths;
        std::vector<MacroInput> inputs;
        std::string error;
    };

    struct TickWindow {
        bool valid = false;
        int left = 0;
        int right = 0;
        int width = 0;
    };

    struct InputResult {
        MacroInput input;
        bool analyzed = false;
        bool ignored = false;
        std::string reason;
        std::string bucket = "invalid";
        TickWindow window;
        std::array<bool, kProbeRadius * 2 + 1> offsets {};
    };

    struct GameLayerPointers {
        PlayLayer* playLayer = nullptr;
        GJBaseGameLayer* gameLayer = nullptr;
    };

    class BinaryReader {
    public:
        explicit BinaryReader(std::vector<uint8_t> data) : m_data(std::move(data)) {}

        size_t remaining() const {
            return m_pos <= m_data.size() ? m_data.size() - m_pos : 0;
        }

        size_t position() const {
            return m_pos;
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
    bool s_simulating = false;
    bool s_creatingDetachedLayer = false;
    bool s_analyzedThisLayer = false;
    MacroReplay s_macro;
    std::filesystem::path s_macroPath = kDefaultMacroPath;
    std::vector<InputResult> s_results;
    std::deque<std::string> s_debugLines;
    std::string s_lastFrameWindowText = "macro not analyzed";
    size_t s_liveDisplayIndex = 0;
    size_t s_nextAnalysisIndex = 0;
    bool s_analysisRunning = false;
    bool s_hasActiveResult = false;
    int s_activeOffset = -kProbeRadius;

    void pushDebugLine(char const* level, std::string const& message) {
        auto line = fmt::format("{} {}", level, message);
        if (line.size() > 520) line = line.substr(0, 517) + "...";
        s_debugLines.push_back(line);
        while (s_debugLines.size() > kMaxDebugLines) s_debugLines.pop_front();
    }

    template <class... Args>
    void fcInfo(fmt::format_string<Args...> fmtString, Args&&... args) {
        auto message = fmt::format(fmtString, std::forward<Args>(args)...);
        pushDebugLine("[I]", message);
        log::info("{}", message);
    }

    template <class... Args>
    void fcDebug(fmt::format_string<Args...> fmtString, Args&&... args) {
        auto message = fmt::format(fmtString, std::forward<Args>(args)...);
        pushDebugLine("[D]", message);
        log::debug("{}", message);
    }

    template <class... Args>
    void fcWarn(fmt::format_string<Args...> fmtString, Args&&... args) {
        auto message = fmt::format(fmtString, std::forward<Args>(args)...);
        pushDebugLine("[W]", message);
        log::warn("{}", message);
    }

    std::string boolText(bool value) {
        return value ? "true" : "false";
    }

    std::string buttonName(uint8_t button) {
        switch (static_cast<PlayerButton>(button)) {
            case PlayerButton::Jump: return "Jump";
            case PlayerButton::Left: return "Left";
            case PlayerButton::Right: return "Right";
            default: return fmt::format("Unknown({})", button);
        }
    }

    std::string modeName(PlayerObject* player) {
        if (!player) return "null-player";
        if (player->m_isShip) return "ship";
        if (player->m_isBird) return "ufo";
        if (player->m_isBall) return "ball";
        if (player->m_isDart) return "wave";
        if (player->m_isRobot) return "robot";
        if (player->m_isSpider) return "spider";
        if (player->m_isSwing) return "swing";
        return "cube";
    }

    bool releaseIsRelevant(PlayerObject* player) {
        if (!player) return false;
        return player->m_isShip || player->m_isDart || player->m_isRobot || player->m_isSwing;
    }

    PlayerObject* inputPlayer(GJBaseGameLayer* layer, MacroInput const& input) {
        if (!layer) return nullptr;
        return input.player2 ? layer->m_player2 : layer->m_player1;
    }

    bool layerIsDead(GJBaseGameLayer* layer) {
        if (!layer) return true;
        auto p1Dead = layer->m_player1 && layer->m_player1->m_isDead;
        auto p2Dead = layer->m_player2 && layer->m_player2->m_isDead;
        return layer->m_playerDied || p1Dead || p2Dead;
    }

    GameLayerPointers captureGameLayerPointers() {
        auto manager = GameManager::get();
        if (!manager) return {};

        return {
            .playLayer = manager->m_playLayer,
            .gameLayer = manager->m_gameLayer,
        };
    }

    void restoreGameLayerPointers(GameLayerPointers const& pointers) {
        auto manager = GameManager::get();
        if (!manager) return;

        manager->m_playLayer = pointers.playLayer;
        manager->m_gameLayer = pointers.gameLayer;
    }

    class ScopedGameLayerPointers final {
    public:
        explicit ScopedGameLayerPointers(PlayLayer* activeLayer) : m_previous(captureGameLayerPointers()) {
            auto manager = GameManager::get();
            if (!manager || !activeLayer) return;

            manager->m_playLayer = activeLayer;
            manager->m_gameLayer = activeLayer;
        }

        ~ScopedGameLayerPointers() {
            restoreGameLayerPointers(m_previous);
        }

    private:
        GameLayerPointers m_previous;
    };

    class ScopedSimulationFlag final {
    public:
        ScopedSimulationFlag() : m_previous(s_simulating) {
            s_simulating = true;
        }

        ~ScopedSimulationFlag() {
            s_simulating = m_previous;
        }

    private:
        bool m_previous = false;
    };

    void setLabelText(std::string const& text) {
        s_lastFrameWindowText = text;
        if (s_label) {
            s_label->setString(text.c_str());
            s_label->limitLabelWidth(240.f, 0.32f, 0.16f);
        }
    }

    std::string displayForWidth(std::optional<int> width) {
        if (!width.has_value() || *width <= 0) return "invalid";
        if (*width > kMaxDisplayWindow) return ">5f";
        return fmt::format("{}f", *width);
    }

    TickWindow tickWindowAroundZero(std::array<bool, kProbeRadius * 2 + 1> const& results) {
        TickWindow window;
        if (!results[kProbeRadius]) return window;

        window.valid = true;
        while (window.left > -kProbeRadius && results[window.left - 1 + kProbeRadius]) --window.left;
        while (window.right < kProbeRadius && results[window.right + 1 + kProbeRadius]) ++window.right;
        window.width = window.right - window.left + 1;
        return window;
    }

    std::optional<int> contiguousWindowAroundZero(std::array<bool, kProbeRadius * 2 + 1> const& results) {
        auto window = tickWindowAroundZero(results);
        if (!window.valid) return std::nullopt;
        return window.width;
    }

    std::string tickWindowText(TickWindow const& window) {
        if (!window.valid) return "invalid";
        auto range = window.left == window.right
            ? fmt::format("{:+}", window.left)
            : fmt::format("{:+}..{:+}", window.left, window.right);
        return fmt::format("ticks {} ({}t)", range, window.width);
    }

    std::string resultMapText(std::array<bool, kProbeRadius * 2 + 1> const& results) {
        std::string text;
        for (int offset = -kProbeRadius; offset <= kProbeRadius; ++offset) {
            if (!text.empty()) text += ' ';
            text += fmt::format("{:+}:{}", offset, results[offset + kProbeRadius] ? "ok" : "--");
        }
        return text;
    }

    std::string buildResultsLabel() {
        if (!s_macro.loaded) return "GDR2: load failed";
        if (s_results.empty()) return "GDR2: no results";

        size_t analyzed = 0;
        size_t ignored = 0;
        for (auto const& result : s_results) {
            if (result.analyzed) ++analyzed;
            if (result.ignored) ++ignored;
        }

        std::ostringstream out;
        out << "GDR2 " << analyzed << " checked, " << ignored << " ignored";
        size_t rows = 0;
        for (auto const& result : s_results) {
            if (rows >= kMaxLabelRows) break;
            out << "\n#" << result.input.ordinal << " f" << result.input.frame << " ";
            out << (result.ignored ? "ignored" : result.bucket);
            ++rows;
        }
        if (s_results.size() > rows) out << "\n... Copy FC for all";
        return out.str();
    }

    std::string buildAnalysisDoneLabel() {
        if (!s_macro.loaded) return "Analysis failed\nmacro not loaded";
        if (s_results.empty()) return "Analysis done\nno results";

        size_t analyzed = 0;
        size_t ignored = 0;
        for (auto const& result : s_results) {
            if (result.analyzed) ++analyzed;
            if (result.ignored) ++ignored;
        }

        return fmt::format(
            "Analysis done\n{} checked, {} ignored\nrun to display inputs",
            analyzed,
            ignored
        );
    }

    std::string buildAnalysisBatchLabel() {
        if (!s_macro.loaded) return "Analysis failed\nmacro not loaded";

        size_t analyzed = 0;
        size_t ignored = 0;
        for (auto const& result : s_results) {
            if (result.analyzed) ++analyzed;
            if (result.ignored) ++ignored;
        }

        return fmt::format(
            "Analysis paused\n{}/{} inputs\n{} checked, {} ignored\nAnalyze FC resumes",
            s_nextAnalysisIndex,
            s_macro.inputs.size(),
            analyzed,
            ignored
        );
    }

    std::string progressBar(size_t done, size_t total) {
        constexpr size_t cells = 18;
        auto filled = total == 0 ? cells : std::min(cells, done * cells / total);
        return fmt::format(
            "[{}{}] {}/{}",
            std::string(filled, '#'),
            std::string(cells - filled, '-'),
            done,
            total
        );
    }

    std::string buildAnalysisRunningLabel(std::string const& detail) {
        auto total = s_macro.loaded ? s_macro.inputs.size() : 0;
        std::ostringstream out;
        out << "Analyzing FC\n" << progressBar(s_nextAnalysisIndex, total);
        if (!detail.empty()) out << "\n" << detail;
        out << "\nleave level: cache kept";
        return out.str();
    }

    std::string resultStatusText(InputResult const& result) {
        if (result.ignored) return fmt::format("ignored ({})", result.reason);
        if (!result.analyzed) return "invalid";
        return fmt::format("{} {}", result.bucket, tickWindowText(result.window));
    }

    std::string buildLiveInputLabel(InputResult const& result, size_t resultIndex) {
        std::ostringstream out;
        out << "FC live " << (resultIndex + 1) << "/" << s_results.size();
        out << "\n#" << result.input.ordinal
            << " f" << result.input.frame
            << " " << (result.input.down ? "down" : "up")
            << " " << buttonName(result.input.button);
        out << "\n" << resultStatusText(result);
        return out.str();
    }

    bool resultMatchesLiveInput(InputResult const& result, bool down, int button, bool isPlayer1) {
        return result.input.down == down
            && result.input.button == static_cast<uint8_t>(button)
            && result.input.player2 == !isPlayer1;
    }

    std::optional<size_t> nextResultIndexForLiveInput(bool down, int button, bool isPlayer1) {
        if (s_liveDisplayIndex >= s_results.size()) return std::nullopt;

        for (auto index = s_liveDisplayIndex; index < s_results.size(); ++index) {
            if (resultMatchesLiveInput(s_results[index], down, button, isPlayer1)) {
                return index;
            }
        }

        // Keep the debug display moving even when the player makes an extra or mismatched input.
        return s_liveDisplayIndex;
    }

    std::string buildResultsText() {
        std::ostringstream out;
        out << "FrameCheck macro results\n";
        out << "Macro: " << s_macroPath.string() << "\n";
        if (!s_macro.loaded) {
            out << "Load failed: " << s_macro.error << "\n";
            return out.str();
        }

        out << "Replay: author=" << s_macro.author
            << " bot=" << s_macro.botName << " v" << s_macro.botVersion
            << " level=" << s_macro.levelName << " id=" << s_macro.levelID
            << " gameVersion=" << s_macro.gameVersion
            << " fps=" << s_macro.framerate
            << " platformer=" << boolText(s_macro.platformer)
            << " totalInputs=" << s_macro.inputs.size()
            << "\n\n";

        for (auto const& result : s_results) {
            out << "#" << result.input.ordinal
                << " frame=" << result.input.frame
                << " button=" << buttonName(result.input.button)
                << " down=" << boolText(result.input.down)
                << " player2=" << boolText(result.input.player2)
                << " -> ";

            if (result.ignored) {
                out << "ignored (" << result.reason << ")";
            }
            else {
                out << result.bucket << " " << tickWindowText(result.window)
                    << " map=" << resultMapText(result.offsets);
            }
            out << "\n";
        }

        return out.str();
    }

    std::vector<std::string> splitCacheLine(std::string const& line) {
        std::vector<std::string> parts;
        std::string part;
        std::istringstream input(line);
        while (std::getline(input, part, '|')) parts.push_back(part);
        return parts;
    }

    std::string cacheFingerprint() {
        auto firstFrame = s_macro.inputs.empty() ? 0 : s_macro.inputs.front().frame;
        auto lastFrame = s_macro.inputs.empty() ? 0 : s_macro.inputs.back().frame;
        return fmt::format(
            "{}|{}|{}|{}|{}",
            s_macroPath.string(),
            s_macro.levelID,
            s_macro.inputs.size(),
            firstFrame,
            lastFrame
        );
    }

    std::string offsetBits(std::array<bool, kProbeRadius * 2 + 1> const& offsets) {
        std::string bits;
        bits.reserve(offsets.size());
        for (auto value : offsets) bits += value ? '1' : '0';
        return bits;
    }

    bool parseBoolCache(std::string const& value) {
        return value == "1" || value == "true";
    }

    void saveAnalysisCache() {
        if (!s_macro.loaded) return;

        std::ostringstream out;
        out << "FPCACHE3\n";
        out << cacheFingerprint() << "\n";
        for (auto const& result : s_results) {
            out << "R|"
                << result.input.ordinal << "|"
                << result.input.frame << "|"
                << static_cast<int>(result.input.button) << "|"
                << (result.input.player2 ? 1 : 0) << "|"
                << (result.input.down ? 1 : 0) << "|"
                << (result.analyzed ? 1 : 0) << "|"
                << (result.ignored ? 1 : 0) << "|"
                << result.bucket << "|"
                << result.reason << "|"
                << (result.window.valid ? 1 : 0) << "|"
                << result.window.left << "|"
                << result.window.right << "|"
                << result.window.width << "|"
                << offsetBits(result.offsets)
                << "\n";
        }

        Mod::get()->setSavedValue<std::string>(kCacheKey, out.str());
        fcDebug("[FrameCheck] saved analysis cache results={} nextIndex={}", s_results.size(), s_nextAnalysisIndex);
    }

    bool loadAnalysisCache() {
        s_results.clear();
        s_nextAnalysisIndex = 0;

        auto text = Mod::get()->getSavedValue<std::string>(kCacheKey, "");
        if (text.empty()) {
            fcInfo("[FrameCheck] no saved analysis cache");
            return false;
        }

        std::istringstream input(text);
        std::string version;
        std::string fingerprint;
        if (!std::getline(input, version) || !std::getline(input, fingerprint) || version != "FPCACHE3") {
            fcWarn("[FrameCheck] ignored saved analysis cache: bad header");
            return false;
        }

        if (fingerprint != cacheFingerprint()) {
            fcInfo("[FrameCheck] saved analysis cache mismatch; expected='{}' got='{}'", cacheFingerprint(), fingerprint);
            return false;
        }

        std::string line;
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            auto parts = splitCacheLine(line);
            if (parts.size() < 15 || parts[0] != "R") {
                fcWarn("[FrameCheck] skipped malformed cache line='{}'", line);
                continue;
            }

            try {
                InputResult result;
                result.input.ordinal = static_cast<size_t>(std::stoull(parts[1]));
                result.input.frame = std::stoull(parts[2]);
                result.input.button = static_cast<uint8_t>(std::stoul(parts[3]));
                result.input.player2 = parseBoolCache(parts[4]);
                result.input.down = parseBoolCache(parts[5]);
                result.analyzed = parseBoolCache(parts[6]);
                result.ignored = parseBoolCache(parts[7]);
                result.bucket = parts[8];
                result.reason = parts[9];
                result.window.valid = parseBoolCache(parts[10]);
                result.window.left = std::stoi(parts[11]);
                result.window.right = std::stoi(parts[12]);
                result.window.width = std::stoi(parts[13]);

                auto const& bits = parts[14];
                for (size_t index = 0; index < result.offsets.size() && index < bits.size(); ++index) {
                    result.offsets[index] = bits[index] == '1';
                }

                s_results.push_back(result);
            }
            catch (std::exception const& error) {
                fcWarn("[FrameCheck] skipped cache line parse error='{}' line='{}'", error.what(), line);
            }
        }

        s_nextAnalysisIndex = std::min(s_results.size(), s_macro.inputs.size());
        s_liveDisplayIndex = 0;
        fcInfo(
            "[FrameCheck] loaded analysis cache results={} totalInputs={} complete={}",
            s_results.size(),
            s_macro.inputs.size(),
            boolText(s_nextAnalysisIndex >= s_macro.inputs.size() && !s_macro.inputs.empty())
        );
        return !s_results.empty();
    }

    PlayLayer* createDetachedLayer(PlayLayer* source, std::string const& label) {
        if (!source || !source->m_level) {
            fcWarn("[FrameCheck/Test] {} cannot create detached layer: missing source level", label);
            return nullptr;
        }

        auto previousPointers = captureGameLayerPointers();
        s_creatingDetachedLayer = true;
        auto* detached = PlayLayer::create(source->m_level, false, false);
        s_creatingDetachedLayer = false;
        restoreGameLayerPointers(previousPointers);

        if (!detached) {
            fcWarn("[FrameCheck/Test] {} detached PlayLayer creation failed", label);
            return nullptr;
        }

        detached->retain();
        detached->setVisible(false);
        detached->m_recordInputs = false;
        detached->m_useReplay = false;
        {
            ScopedGameLayerPointers scopedPointers(detached);
            detached->startGame();
            detached->resume();
            detached->m_recordInputs = false;
            detached->m_useReplay = false;
        }
        fcDebug("[FrameCheck/Test] {} detached PlayLayer ready ptr={}", label, static_cast<void*>(detached));
        return detached;
    }

    void releaseDetachedLayer(PlayLayer*& detached, std::string const& label) {
        if (!detached) return;

        fcDebug("[FrameCheck/Test] {} releasing detached PlayLayer ptr={}", label, static_cast<void*>(detached));
        detached->removeFromParentAndCleanup(true);
        detached->release();
        detached = nullptr;
    }

    void applyMacroInput(PlayLayer* layer, MacroInput const& input, char const* prefix) {
        fcDebug(
            "{} applying input#{} frame={} down={} button={}({}) player2={} mode={} time={:.6f}",
            prefix,
            input.ordinal,
            input.frame,
            boolText(input.down),
            input.button,
            buttonName(input.button),
            boolText(input.player2),
            modeName(inputPlayer(layer, input)),
            layer->m_gameState.m_levelTime
        );
        layer->handleButton(input.down, input.button, !input.player2);
    }

    bool stepDetached(PlayLayer* layer, uint64_t frame, double dt, char const* prefix) {
        layer->update(static_cast<float>(dt));
        auto dead = layerIsDead(layer);
        if (dead || frame % 120 == 0) {
            fcDebug("{} stepped frame={} dt={:.8f} time={:.6f} dead={}", prefix, frame, dt, layer->m_gameState.m_levelTime, boolText(dead));
        }
        return !dead;
    }

    struct PlaybackEvent {
        uint64_t frame = 0;
        size_t inputIndex = 0;
    };

    struct BranchRun {
        PlayLayer* layer = nullptr;
        std::vector<PlaybackEvent> events;
        size_t eventIndex = 0;
        uint64_t frame = 0;
        uint64_t stopFrame = 0;
        double dt = 1.0 / kFallbackFramerate;
        size_t targetInputIndex = 0;
        int offset = 0;
        std::string label;
    };

    InputResult s_activeResult;
    std::optional<BranchRun> s_branchRun;

    bool replayMacroBranch(
        PlayLayer* layer,
        MacroReplay const& replay,
        std::optional<size_t> targetInputIndex,
        int targetOffset,
        uint64_t stopFrame,
        char const* prefix
    ) {
        std::vector<PlaybackEvent> events;
        events.reserve(replay.inputs.size());

        for (size_t index = 0; index < replay.inputs.size(); ++index) {
            auto adjustedFrame = replay.inputs[index].frame;
            if (targetInputIndex.has_value() && index == *targetInputIndex) {
                auto shifted = static_cast<int64_t>(adjustedFrame) + targetOffset;
                if (shifted < 0) {
                    fcInfo("{} target input#{} offset={:+} shifted before frame 0", prefix, replay.inputs[index].ordinal, targetOffset);
                    return false;
                }
                adjustedFrame = static_cast<uint64_t>(shifted);
            }
            if (adjustedFrame <= stopFrame) events.push_back({adjustedFrame, index});
        }

        std::stable_sort(events.begin(), events.end(), [&](PlaybackEvent const& a, PlaybackEvent const& b) {
            if (a.frame != b.frame) return a.frame < b.frame;
            return replay.inputs[a.inputIndex].ordinal < replay.inputs[b.inputIndex].ordinal;
        });

        auto dt = replay.framerate > 1.0 ? 1.0 / replay.framerate : 1.0 / kFallbackFramerate;
        ScopedGameLayerPointers scopedPointers(layer);
        size_t eventIndex = 0;

        for (uint64_t frame = 0; frame <= stopFrame; ++frame) {
            while (eventIndex < events.size() && events[eventIndex].frame == frame) {
                applyMacroInput(layer, replay.inputs[events[eventIndex].inputIndex], prefix);
                if (layerIsDead(layer)) {
                    fcInfo("{} failed immediately after input at frame={}", prefix, frame);
                    return false;
                }
                ++eventIndex;
            }

            if (frame == stopFrame) break;
            if (!stepDetached(layer, frame, dt, prefix)) return false;
        }

        return !layerIsDead(layer);
    }

    bool releaseRelevantAtInput(PlayLayer* source, MacroReplay const& replay, size_t targetInputIndex) {
        auto const& input = replay.inputs[targetInputIndex];
        auto label = fmt::format("input#{} relevance", input.ordinal);
        auto* detached = createDetachedLayer(source, label);
        if (!detached) return false;

        MacroReplay priorReplay = replay;
        priorReplay.inputs.clear();
        for (auto const& other : replay.inputs) {
            if (other.frame < input.frame || (other.frame == input.frame && other.ordinal < input.ordinal)) {
                priorReplay.inputs.push_back(other);
            }
        }

        s_simulating = true;
        auto ok = replayMacroBranch(detached, priorReplay, std::nullopt, 0, input.frame, "[FrameCheck/Test]");
        s_simulating = false;

        auto* player = ok ? inputPlayer(detached, input) : nullptr;
        auto relevant = releaseIsRelevant(player);
        fcDebug(
            "[FrameCheck/Test] input#{} release relevance ok={} mode={} relevant={}",
            input.ordinal,
            boolText(ok),
            modeName(player),
            boolText(relevant)
        );
        releaseDetachedLayer(detached, label);
        return relevant;
    }

    bool testOffset(PlayLayer* source, MacroReplay const& replay, size_t targetInputIndex, int offset) {
        auto const& input = replay.inputs[targetInputIndex];
        auto shiftedFrame = static_cast<int64_t>(input.frame) + offset;
        if (shiftedFrame < 0) {
            fcInfo("[FrameCheck/Test] input#{} offset={:+} fail reason=before-frame-zero", input.ordinal, offset);
            return false;
        }

        auto stopFrame = std::max<uint64_t>(input.frame, static_cast<uint64_t>(shiftedFrame)) + kPostInputTicks;
        auto label = fmt::format("input#{} offset={:+}", input.ordinal, offset);
        auto* detached = createDetachedLayer(source, label);
        if (!detached) {
            fcInfo("[FrameCheck/Test] input#{} offset={:+} fail reason=no-detached-layer", input.ordinal, offset);
            return false;
        }

        s_simulating = true;
        auto ok = replayMacroBranch(detached, replay, targetInputIndex, offset, stopFrame, "[FrameCheck/Test]");
        s_simulating = false;

        fcInfo(
            "[FrameCheck/Test] input#{} offset={:+} shiftedFrame={} stopFrame={} result={}",
            input.ordinal,
            offset,
            static_cast<uint64_t>(shiftedFrame),
            stopFrame,
            ok ? "success" : "fail"
        );
        releaseDetachedLayer(detached, label);
        return ok;
    }

    InputResult analyzeInput(PlayLayer* source, MacroReplay const& replay, size_t inputIndex) {
        auto const& input = replay.inputs[inputIndex];
        InputResult result;
        result.input = input;

        fcInfo(
            "[FrameCheck] analyzing input#{} frame={} down={} button={}({}) player2={}",
            input.ordinal,
            input.frame,
            boolText(input.down),
            input.button,
            buttonName(input.button),
            boolText(input.player2)
        );
        if (!input.down) {
            fcInfo("[FrameCheck] release input#{} will be analyzed for timing offsets", input.ordinal);
        }

        if (input.button != static_cast<uint8_t>(PlayerButton::Jump)) {
            result.ignored = true;
            result.reason = "non-jump-button";
            fcInfo("[FrameCheck] ignored input#{} reason={}", input.ordinal, result.reason);
            return result;
        }

        result.analyzed = true;
        if (!input.down) {
            fcInfo("[FrameCheck] release input#{} will be branch-checked for timing offsets", input.ordinal);
        }
        for (int offset = -kProbeRadius; offset <= kProbeRadius; ++offset) {
            auto success = testOffset(source, replay, inputIndex, offset);
            result.offsets[offset + kProbeRadius] = success;
            fcInfo("[FrameCheck/Test] input#{} tested offset={:+} result={}", input.ordinal, offset, success ? "success" : "fail");
        }

        auto width = contiguousWindowAroundZero(result.offsets);
        result.window = tickWindowAroundZero(result.offsets);
        result.bucket = displayForWidth(width);

        fcInfo(
            "[FrameCheck/Result] input#{} finalWindow={} bucket={} map={}",
            input.ordinal,
            tickWindowText(result.window),
            result.bucket,
            resultMapText(result.offsets)
        );
        fcInfo("[FrameCheck/Result] input#{} stored result frame={} result={}", input.ordinal, input.frame, result.bucket);
        return result;
    }

    void clearBranchRun() {
        if (!s_branchRun.has_value()) return;

        auto label = s_branchRun->label;
        auto* detached = s_branchRun->layer;
        releaseDetachedLayer(detached, label);
        s_branchRun.reset();
    }

    void resetAnalysisJob(bool clearActiveOnly) {
        clearBranchRun();
        s_analysisRunning = false;
        s_hasActiveResult = false;
        s_activeOffset = -kProbeRadius;
        if (!clearActiveOnly) {
            s_results.clear();
            s_nextAnalysisIndex = 0;
            s_liveDisplayIndex = 0;
        }
    }

    bool shouldIgnoreForIncrementalAnalysis(MacroInput const& input, InputResult& result) {
        if (input.button != static_cast<uint8_t>(PlayerButton::Jump)) {
            result.ignored = true;
            result.reason = "non-jump-button";
            return true;
        }

        if (!input.down) {
            fcInfo("[FrameCheck] release input#{} accepted for branch timing checks", input.ordinal);
        }

        return false;
    }

    bool startBranchRun(PlayLayer* source, MacroReplay const& replay, size_t targetInputIndex, int offset) {
        auto const& input = replay.inputs[targetInputIndex];
        auto shiftedFrame = static_cast<int64_t>(input.frame) + offset;
        if (shiftedFrame < 0) {
            fcInfo("[FrameCheck/Test] input#{} offset={:+} fail reason=before-frame-zero", input.ordinal, offset);
            return false;
        }

        auto stopFrame = std::max<uint64_t>(input.frame, static_cast<uint64_t>(shiftedFrame)) + kPostInputTicks;
        auto label = fmt::format("input#{} offset={:+}", input.ordinal, offset);
        auto* detached = createDetachedLayer(source, label);
        if (!detached) {
            fcInfo("[FrameCheck/Test] input#{} offset={:+} fail reason=no-detached-layer", input.ordinal, offset);
            return false;
        }

        BranchRun run;
        run.layer = detached;
        run.targetInputIndex = targetInputIndex;
        run.offset = offset;
        run.stopFrame = stopFrame;
        run.dt = replay.framerate > 1.0 ? 1.0 / replay.framerate : 1.0 / kFallbackFramerate;
        run.label = label;
        run.events.reserve(replay.inputs.size());

        for (size_t index = 0; index < replay.inputs.size(); ++index) {
            auto adjustedFrame = replay.inputs[index].frame;
            if (index == targetInputIndex) adjustedFrame = static_cast<uint64_t>(shiftedFrame);
            if (adjustedFrame <= stopFrame) run.events.push_back({adjustedFrame, index});
        }

        std::stable_sort(run.events.begin(), run.events.end(), [&](PlaybackEvent const& a, PlaybackEvent const& b) {
            if (a.frame != b.frame) return a.frame < b.frame;
            return replay.inputs[a.inputIndex].ordinal < replay.inputs[b.inputIndex].ordinal;
        });

        fcInfo(
            "[FrameCheck/Test] input#{} {} offset={:+} queued shiftedFrame={} stopFrame={} events={}",
            input.ordinal,
            input.down ? "press" : "release",
            offset,
            static_cast<uint64_t>(shiftedFrame),
            stopFrame,
            run.events.size()
        );
        s_branchRun = std::move(run);
        setLabelText(buildAnalysisRunningLabel(fmt::format(
            "#{} f{} {} offset {:+}\nbranch 0/{}",
            input.ordinal,
            input.frame,
            input.down ? "down" : "up",
            offset,
            stopFrame
        )));
        return true;
    }

    std::optional<bool> stepBranchRun(size_t maxSteps) {
        if (!s_branchRun.has_value()) return false;
        auto& run = *s_branchRun;
        if (!run.layer) return false;

        ScopedGameLayerPointers scopedPointers(run.layer);
        ScopedSimulationFlag simulating;

        for (size_t step = 0; step < maxSteps; ++step) {
            while (run.eventIndex < run.events.size() && run.events[run.eventIndex].frame == run.frame) {
                applyMacroInput(run.layer, s_macro.inputs[run.events[run.eventIndex].inputIndex], "[FrameCheck/Test]");
                if (layerIsDead(run.layer)) {
                    fcInfo(
                        "[FrameCheck/Test] input#{} offset={:+} failed immediately after input at frame={}",
                        s_macro.inputs[run.targetInputIndex].ordinal,
                        run.offset,
                        run.frame
                    );
                    return false;
                }
                ++run.eventIndex;
            }

            if (run.frame == run.stopFrame) {
                return !layerIsDead(run.layer);
            }

            if (!stepDetached(run.layer, run.frame, run.dt, "[FrameCheck/Test]")) {
                fcInfo(
                    "[FrameCheck/Test] input#{} offset={:+} failed while stepping frame={}",
                    s_macro.inputs[run.targetInputIndex].ordinal,
                    run.offset,
                    run.frame
                );
                return false;
            }

            ++run.frame;
        }

        auto const& input = s_macro.inputs[run.targetInputIndex];
        setLabelText(buildAnalysisRunningLabel(fmt::format(
            "#{} f{} {} offset {:+}\nbranch {}/{}",
            input.ordinal,
            input.frame,
            input.down ? "down" : "up",
            run.offset,
            run.frame,
            run.stopFrame
        )));
        return std::nullopt;
    }

    void storeActiveResult() {
        auto width = contiguousWindowAroundZero(s_activeResult.offsets);
        s_activeResult.window = tickWindowAroundZero(s_activeResult.offsets);
        s_activeResult.bucket = displayForWidth(width);

        fcInfo(
            "[FrameCheck/Result] input#{} finalWindow={} bucket={} map={}",
            s_activeResult.input.ordinal,
            tickWindowText(s_activeResult.window),
            s_activeResult.bucket,
            resultMapText(s_activeResult.offsets)
        );
        fcInfo(
            "[FrameCheck/Result] input#{} stored result frame={} result={}",
            s_activeResult.input.ordinal,
            s_activeResult.input.frame,
            s_activeResult.bucket
        );

        s_results.push_back(s_activeResult);
        s_nextAnalysisIndex = std::min(s_results.size(), s_macro.inputs.size());
        s_liveDisplayIndex = 0;
        s_hasActiveResult = false;
        s_activeOffset = -kProbeRadius;
        saveAnalysisCache();
    }

    void finishCurrentOffset(bool success) {
        if (!s_hasActiveResult) return;

        auto const& input = s_activeResult.input;
        s_activeResult.offsets[s_activeOffset + kProbeRadius] = success;
        fcInfo("[FrameCheck/Test] input#{} tested offset={:+} result={}", input.ordinal, s_activeOffset, success ? "success" : "fail");
        clearBranchRun();

        if (s_activeOffset >= kProbeRadius) {
            storeActiveResult();
            return;
        }

        ++s_activeOffset;
        setLabelText(buildAnalysisRunningLabel(fmt::format(
            "#{} f{} {} next offset {:+}",
            input.ordinal,
            input.frame,
            input.down ? "down" : "up",
            s_activeOffset
        )));
    }

    void beginCurrentInput() {
        auto const& input = s_macro.inputs[s_nextAnalysisIndex];
        s_activeResult = {};
        s_activeResult.input = input;
        s_hasActiveResult = true;
        s_activeOffset = -kProbeRadius;

        fcInfo(
            "[FrameCheck] analyzing input#{} frame={} down={} button={}({}) player2={}",
            input.ordinal,
            input.frame,
            boolText(input.down),
            input.button,
            buttonName(input.button),
            boolText(input.player2)
        );
        if (!input.down) {
            fcInfo("[FrameCheck] release input#{} will be analyzed for timing offsets", input.ordinal);
        }

        if (shouldIgnoreForIncrementalAnalysis(input, s_activeResult)) {
            fcInfo("[FrameCheck] ignored input#{} reason={}", input.ordinal, s_activeResult.reason);
            s_results.push_back(s_activeResult);
            s_nextAnalysisIndex = std::min(s_results.size(), s_macro.inputs.size());
            s_liveDisplayIndex = 0;
            s_hasActiveResult = false;
            saveAnalysisCache();
            setLabelText(buildAnalysisRunningLabel(fmt::format(
                "#{} f{} ignored",
                input.ordinal,
                input.frame
            )));
            return;
        }

        s_activeResult.analyzed = true;
        setLabelText(buildAnalysisRunningLabel(fmt::format(
            "#{} f{} {} starting",
            input.ordinal,
            input.frame,
            input.down ? "down" : "up"
        )));
    }

    void finishAnalysisIfDone() {
        if (!s_macro.loaded || s_nextAnalysisIndex < s_macro.inputs.size()) return;

        s_analysisRunning = false;
        s_hasActiveResult = false;
        clearBranchRun();
        saveAnalysisCache();
        setLabelText(buildAnalysisDoneLabel());
        fcInfo("[FrameCheck] analysis complete resultsStored={}", s_results.size());
    }

    void tickAnalysisWork(PlayLayer* source) {
        if (!s_analysisRunning) return;

        if (!source) {
            setLabelText("Analyzing FC\nwaiting for level");
            return;
        }

        if (!s_macro.loaded) {
            fcWarn("[FrameCheck] analysis stopped: macro not loaded");
            s_analysisRunning = false;
            setLabelText("Analysis failed\nmacro not loaded");
            return;
        }

        if (source->m_isPlatformer || s_macro.platformer) {
            auto reason = source->m_isPlatformer ? "current-level-platformer" : "macro-platformer";
            fcWarn("[FrameCheck] analysis stopped reason={}", reason);
            s_analysisRunning = false;
            setLabelText(fmt::format("FrameCheck invalid: {}", reason));
            return;
        }

        finishAnalysisIfDone();
        if (!s_analysisRunning) return;

        // Drain cheap ignored inputs quickly; branch work is still chunked below.
        for (size_t ignoredBudget = 0; ignoredBudget < 8 && s_analysisRunning && !s_hasActiveResult; ++ignoredBudget) {
            beginCurrentInput();
            finishAnalysisIfDone();
            if (s_hasActiveResult) break;
        }

        if (!s_analysisRunning || !s_hasActiveResult) return;

        if (!s_branchRun.has_value()) {
            if (!startBranchRun(source, s_macro, s_nextAnalysisIndex, s_activeOffset)) {
                finishCurrentOffset(false);
            }
            return;
        }

        auto completed = stepBranchRun(kBranchStepsPerTick);
        if (!completed.has_value()) return;

        finishCurrentOffset(*completed);
        finishAnalysisIfDone();
    }

    std::optional<MacroReplay> parseGdr2File(std::filesystem::path const& path, std::string& error) {
        fcInfo("[MacroParser] macro load path={}", path.string());

        std::ifstream file(path, std::ios::binary);
        if (!file) {
            error = "failed to open macro file";
            fcWarn("[MacroParser] {}", error);
            return std::nullopt;
        }

        file.seekg(0, std::ios::end);
        auto size = static_cast<size_t>(file.tellg());
        file.seekg(0, std::ios::beg);

        std::vector<uint8_t> bytes(size);
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!file) {
            error = "failed to read macro file";
            fcWarn("[MacroParser] {}", error);
            return std::nullopt;
        }

        fcInfo("[MacroParser] read {} bytes", bytes.size());
        BinaryReader reader(std::move(bytes));

        uint8_t magic[3] {};
        if (!reader.readBytes(magic, sizeof(magic)) || std::string_view(reinterpret_cast<char*>(magic), 3) != "GDR") {
            error = "invalid GDR magic";
            fcWarn("[MacroParser] {}", error);
            return std::nullopt;
        }

        MacroReplay replay;
        auto version = reader.readVarUInt();
        if (!version.has_value()) {
            error = "missing GDR version";
            fcWarn("[MacroParser] {}", error);
            return std::nullopt;
        }
        replay.version = static_cast<int>(*version);

        auto readVarIntField = [&](char const* name, int& out) -> bool {
            auto value = reader.readVarUInt();
            if (!value.has_value()) {
                error = fmt::format("missing {}", name);
                return false;
            }
            out = static_cast<int>(*value);
            return true;
        };

        if (
            !reader.readString(replay.inputTag) ||
            !reader.readString(replay.author) ||
            !reader.readString(replay.description) ||
            !reader.readFloat(replay.duration) ||
            !readVarIntField("game version", replay.gameVersion) ||
            !reader.readDouble(replay.framerate) ||
            !readVarIntField("seed", replay.seed) ||
            !readVarIntField("coins", replay.coins) ||
            !reader.readBool(replay.ldm) ||
            !reader.readBool(replay.platformer) ||
            !reader.readString(replay.botName) ||
            !readVarIntField("bot version", replay.botVersion)
        ) {
            if (error.empty()) error = "failed while reading replay metadata";
            fcWarn("[MacroParser] {}", error);
            return std::nullopt;
        }

        auto levelID = reader.readVarUInt();
        if (!levelID.has_value() || !reader.readString(replay.levelName)) {
            error = "failed while reading level metadata";
            fcWarn("[MacroParser] {}", error);
            return std::nullopt;
        }
        replay.levelID = static_cast<uint32_t>(*levelID);

        auto extensionSize = reader.readVarUInt();
        if (!extensionSize.has_value() || !reader.skip(static_cast<size_t>(*extensionSize))) {
            error = "invalid replay extension block";
            fcWarn("[MacroParser] {}", error);
            return std::nullopt;
        }

        auto deathCount = reader.readVarUInt();
        if (!deathCount.has_value()) {
            error = "missing death count";
            fcWarn("[MacroParser] {}", error);
            return std::nullopt;
        }

        uint64_t deathFrame = 0;
        for (uint64_t i = 0; i < *deathCount; ++i) {
            auto delta = reader.readVarUInt();
            if (!delta.has_value()) {
                error = "truncated death list";
                fcWarn("[MacroParser] {}", error);
                return std::nullopt;
            }
            deathFrame += *delta;
            replay.deaths.push_back(deathFrame);
        }

        auto inputCount = reader.readVarUInt();
        auto p1InputCount = reader.readVarUInt();
        if (!inputCount.has_value() || !p1InputCount.has_value()) {
            error = "missing input counts";
            fcWarn("[MacroParser] {}", error);
            return std::nullopt;
        }

        fcInfo(
            "[MacroParser] replay metadata version={} tag='{}' author='{}' desc='{}' duration={} gameVersion={} fps={} seed={} coins={} ldm={} platformer={} bot='{}' botVersion={} level='{}' levelID={} deaths={} inputCount={} p1Inputs={} extBytes={}",
            replay.version,
            replay.inputTag,
            replay.author,
            replay.description,
            replay.duration,
            replay.gameVersion,
            replay.framerate,
            replay.seed,
            replay.coins,
            boolText(replay.ldm),
            boolText(replay.platformer),
            replay.botName,
            replay.botVersion,
            replay.levelName,
            replay.levelID,
            replay.deaths.size(),
            *inputCount,
            *p1InputCount,
            *extensionSize
        );

        replay.inputs.reserve(static_cast<size_t>(*inputCount));
        uint64_t runningFrame = 0;
        uint64_t p1Remaining = *p1InputCount;
        auto hasInputExtension = !replay.inputTag.empty();

        while (reader.remaining() > 0 && replay.inputs.size() < *inputCount) {
            auto packed = reader.readVarUInt();
            if (!packed.has_value()) {
                error = "truncated input stream";
                fcWarn("[MacroParser] {}", error);
                return std::nullopt;
            }

            MacroInput input;
            input.player2 = p1Remaining == 0;
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
                    fcWarn("[MacroParser] {}", error);
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

        if (replay.inputs.size() != *inputCount) {
            error = fmt::format("expected {} inputs, parsed {}", *inputCount, replay.inputs.size());
            fcWarn("[MacroParser] {}", error);
            return std::nullopt;
        }

        std::stable_sort(replay.inputs.begin(), replay.inputs.end(), [](MacroInput const& a, MacroInput const& b) {
            if (a.frame != b.frame) return a.frame < b.frame;
            return a.ordinal < b.ordinal;
        });

        fcInfo("[MacroParser] total inputs parsed={}", replay.inputs.size());
        for (auto const& input : replay.inputs) {
            fcDebug(
                "[MacroParser] input#{} frame={} button={}({}) down={} player2={}",
                input.ordinal,
                input.frame,
                input.button,
                buttonName(input.button),
                boolText(input.down),
                boolText(input.player2)
            );
        }

        replay.loaded = true;
        return replay;
    }

    bool loadMacro() {
        resetAnalysisJob(false);
        s_results.clear();
        s_liveDisplayIndex = 0;
        s_nextAnalysisIndex = 0;
        std::string error;
        auto replay = parseGdr2File(s_macroPath, error);
        if (!replay.has_value()) {
            s_macro = {};
            s_macro.error = error;
            setLabelText("GDR2 load failed");
            return false;
        }

        s_macro = std::move(*replay);
        auto loadedCache = loadAnalysisCache();
        setLabelText(loadedCache ? buildResultsLabel() : fmt::format("GDR2 loaded: {} inputs", s_macro.inputs.size()));
        return true;
    }

    bool analyzeMacro(PlayLayer* layer) {
        if (!layer) {
            fcWarn("[FrameCheck] analysis skipped: no active PlayLayer");
            setLabelText("Analysis failed\nno active level");
            return false;
        }

        if (s_macro.loaded && s_nextAnalysisIndex >= s_macro.inputs.size() && !s_results.empty()) {
            fcInfo("[FrameCheck] analysis skipped: all results already available count={}", s_results.size());
            setLabelText(buildAnalysisDoneLabel());
            return true;
        }

        s_analyzedThisLayer = true;

        if (!s_macro.loaded && !loadMacro()) {
            fcWarn("[FrameCheck] analysis skipped: macro load failed");
            setLabelText("Analysis failed\nmacro load failed");
            return false;
        }

        fcInfo(
            "[FrameCheck] analysis start currentLayer={} macroInputs={} currentPlatformer={} macroPlatformer={}",
            static_cast<void*>(layer),
            s_macro.inputs.size(),
            boolText(layer->m_isPlatformer),
            boolText(s_macro.platformer)
        );

        if (layer->m_isPlatformer || s_macro.platformer) {
            auto reason = layer->m_isPlatformer ? "current-level-platformer" : "macro-platformer";
            fcWarn("[FrameCheck] analysis aborted reason={}", reason);
            setLabelText(fmt::format("FrameCheck invalid: {}", reason));
            return false;
        }

        if (s_nextAnalysisIndex == 0 && s_results.empty()) {
            s_results.reserve(s_macro.inputs.size());
        }

        if (s_analysisRunning) {
            fcInfo("[FrameCheck] analysis request ignored: already running nextIndex={}", s_nextAnalysisIndex);
            setLabelText(buildAnalysisRunningLabel("already running"));
            return true;
        }

        s_liveDisplayIndex = 0;
        s_analysisRunning = true;
        s_hasActiveResult = false;
        s_activeOffset = -kProbeRadius;
        clearBranchRun();
        fcInfo(
            "[FrameCheck] analysis queued nextIndex={} totalInputs={} existingResults={}",
            s_nextAnalysisIndex,
            s_macro.inputs.size(),
            s_results.size()
        );
        setLabelText(buildAnalysisRunningLabel("queued"));
        return true;
    }

    void createLabel(PlayLayer* layer) {
        if (!layer) return;

        auto size = cocos2d::CCDirector::sharedDirector()->getWinSize();
        s_label = cocos2d::CCLabelBMFont::create(s_lastFrameWindowText.c_str(), "bigFont.fnt");
        if (!s_label) {
            fcWarn("[FrameCheck] failed to create debug label");
            return;
        }

        s_label->setAnchorPoint({0.f, 1.f});
        s_label->setPosition({6.f, size.height - 6.f});
        s_label->setScale(0.32f);
        s_label->limitLabelWidth(240.f, 0.32f, 0.16f);
        s_label->setColor({255, 80, 80});
        s_label->setZOrder(9999);
        s_label->setID("framecheck-debug-label"_spr);
        layer->addChild(s_label);
    }

    PlayLayer* currentPlayLayer() {
        if (s_layer) return s_layer;

        auto manager = GameManager::get();
        auto* layer = manager ? manager->m_playLayer : nullptr;
        if (!layer) return nullptr;

        s_layer = layer;
        fcWarn("[FrameCheck] recovered active PlayLayer from GameManager ptr={}", static_cast<void*>(s_layer));
        if (!s_label) {
            createLabel(s_layer);
            setLabelText(s_lastFrameWindowText);
        }
        return s_layer;
    }
}

void init() {
    fcInfo("[FrameCheck] mod init: macro frame-window prototype loaded");
    fcInfo("[FrameCheck] config: macroPath='{}' radius={} ticks postInputHorizon={} ticks", s_macroPath.string(), kProbeRadius, kPostInputTicks);
}

void onPlayLayerInit(PlayLayer* layer) {
    if (s_creatingDetachedLayer) {
        fcDebug("[FrameCheck] detached PlayLayer init ignored ptr={}", static_cast<void*>(layer));
        return;
    }

    s_layer = layer;
    s_label = nullptr;
    s_analyzedThisLayer = false;
    s_liveDisplayIndex = 0;
    if (s_lastFrameWindowText.empty()) s_lastFrameWindowText = "GDR2: waiting";
    createLabel(layer);

    fcInfo(
        "[FrameCheck] level init layer={} platformer={} replay={} practice={} testMode={}",
        static_cast<void*>(layer),
        boolText(layer && layer->m_isPlatformer),
        boolText(layer && layer->m_useReplay),
        boolText(layer && layer->m_isPracticeMode),
        boolText(layer && layer->m_isTestMode)
    );

    loadMacro();
}

void onLevelEnter(PlayLayer* layer) {
    if (s_creatingDetachedLayer || layer != s_layer) {
        fcDebug("[FrameCheck] non-live level enter ignored layer={}", static_cast<void*>(layer));
        return;
    }

    fcInfo(
        "[FrameCheck] level enter layer={} platformer={} macroLoaded={}",
        static_cast<void*>(layer),
        boolText(layer && layer->m_isPlatformer),
        boolText(s_macro.loaded)
    );
    s_liveDisplayIndex = 0;
    if (s_analysisRunning) {
        setLabelText(buildAnalysisRunningLabel("running"));
    }
    else {
        setLabelText(s_results.empty() ? "GDR2 loaded\nPause > Analyze FC" : buildResultsLabel());
    }
}

void onLevelExit(PlayLayer* layer) {
    if (s_creatingDetachedLayer || layer != s_layer) {
        fcDebug("[FrameCheck] non-live level exit ignored layer={}", static_cast<void*>(layer));
        return;
    }

    fcInfo("[FrameCheck] level exit layer={} resultsStored={}", static_cast<void*>(layer), s_results.size());
    if (s_analysisRunning || s_branchRun.has_value()) {
        fcWarn("[FrameCheck] stopping in-progress analysis because live level exited; completed results stay cached");
        resetAnalysisJob(true);
    }
    if (s_layer == layer) {
        s_layer = nullptr;
        s_label = nullptr;
    }
}

void onAttemptReset(PlayLayer* layer) {
    fcInfo("[FrameCheck] attempt reset layer={} macro-results-kept={}", static_cast<void*>(layer), s_results.size());
    s_liveDisplayIndex = 0;
    if (!s_results.empty()) setLabelText("FC live ready\nnext input #1");
}

bool loadMacroFile(std::filesystem::path const& path) {
    s_macroPath = path;
    s_analyzedThisLayer = false;
    s_nextAnalysisIndex = 0;
    fcInfo("[FrameCheck] selected macro file path={}", s_macroPath.string());
    return loadMacro();
}

bool analyzeCurrentMacro() {
    auto* layer = currentPlayLayer();
    fcInfo(
        "[FrameCheck] manual analysis requested storedLayer={} activeLayer={}",
        static_cast<void*>(s_layer),
        static_cast<void*>(layer)
    );
    return analyzeMacro(layer);
}

std::filesystem::path currentMacroPath() {
    return s_macroPath;
}

void afterProcessCommands(GJBaseGameLayer* layer, float, bool, bool) {
    if (s_simulating || s_creatingDetachedLayer) return;

    auto* liveLayer = currentPlayLayer();
    if (!liveLayer || layer != static_cast<GJBaseGameLayer*>(liveLayer)) return;

    tickAnalysisWork(liveLayer);
}

void tickAnalysisFromUI() {
    if (s_simulating || s_creatingDetachedLayer) return;
    tickAnalysisWork(currentPlayLayer());
}

void beforeHandleButton(GJBaseGameLayer* layer, bool down, int button, bool isPlayer1) {
    if (s_simulating) {
        fcDebug("[FrameCheck] simulated input passthrough down={} button={} isPlayer1={}", boolText(down), button, boolText(isPlayer1));
        return;
    }

    auto* liveLayer = currentPlayLayer();
    if (s_creatingDetachedLayer || !liveLayer || layer != static_cast<GJBaseGameLayer*>(liveLayer)) {
        fcDebug(
            "[FrameCheck] non-live input ignored layer={} liveLayer={} down={} button={} isPlayer1={}",
            static_cast<void*>(layer),
            static_cast<void*>(liveLayer),
            boolText(down),
            button,
            boolText(isPlayer1)
        );
        return;
    }

    fcDebug(
        "[FrameCheck] live input display request down={} button={}({}) isPlayer1={} cursor={} results={}",
        boolText(down),
        button,
        buttonName(static_cast<uint8_t>(button)),
        boolText(isPlayer1),
        s_liveDisplayIndex,
        s_results.size()
    );

    if (s_results.empty()) {
        setLabelText("GDR2 not analyzed\nPause > Analyze FC");
        fcInfo("[FrameCheck] live input has no precomputed macro results to display");
        return;
    }

    auto resultIndex = nextResultIndexForLiveInput(down, button, isPlayer1);
    if (!resultIndex.has_value()) {
        setLabelText("FC live done\nno more inputs");
        fcInfo("[FrameCheck] live input after all stored results down={} button={} isPlayer1={}", boolText(down), button, boolText(isPlayer1));
        return;
    }

    auto const& result = s_results[*resultIndex];
    if (*resultIndex != s_liveDisplayIndex) {
        fcWarn(
            "[FrameCheck] live input skipped display cursor from {} to {} to match down={} button={} isPlayer1={}",
            s_liveDisplayIndex,
            *resultIndex,
            boolText(down),
            button,
            boolText(isPlayer1)
        );
    }

    setLabelText(buildLiveInputLabel(result, *resultIndex));
    s_liveDisplayIndex = *resultIndex + 1;
    fcInfo(
        "[FrameCheck] displayed stored result input#{} frame={} status='{}' nextCursor={}",
        result.input.ordinal,
        result.input.frame,
        resultStatusText(result),
        s_liveDisplayIndex
    );
}

std::string recentDebugLogText() {
    std::ostringstream output;
    output << "Latest frame window: " << s_lastFrameWindowText << "\n\n";
    output << buildResultsText() << "\n";
    output << "FrameCheck recent debug log (" << s_debugLines.size() << " lines kept)\n\n";

    if (s_debugLines.empty()) {
        output << "[FrameCheck] No debug log lines captured yet.\n";
        return output.str();
    }

    for (auto const& line : s_debugLines) output << line << '\n';
    return output.str();
}

std::string recentAnalysisLogText() {
    std::ostringstream output;
    output << "Latest frame window: " << s_lastFrameWindowText << "\n\n";
    output << buildResultsText();
    return output.str();
}

std::string recentFrameWindowText() {
    return s_lastFrameWindowText;
}

bool isSimulating() {
    return s_simulating;
}
}
