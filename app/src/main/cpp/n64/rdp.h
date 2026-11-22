#pragma once

#include <array>
#include <cstdint>

namespace m64plus::n64 {

class Bus;

struct RdpCommandInfo final {
    std::uint8_t opcode = 0;
    std::uint8_t lengthWords = 1;
    bool knownOpcode = false;
};

// Decodes the command header only. It does not execute rendering operations.
class RdpCommandDecoder final {
public:
    static RdpCommandInfo decode(std::uint64_t header) noexcept;
};

struct RdpCommandPacket final {
    static constexpr std::size_t kMaxWords = 22U;
    RdpCommandInfo info{};
    std::array<std::uint64_t, kMaxWords> words{};
};

class RdpCommandStream final {
public:
    // Fetches one complete decoded command from DPC; failure leaves the packet and cursor unchanged.
    static bool fetchNext(Bus& bus, RdpCommandPacket& packet) noexcept;
};

enum class RdpProcessResult : std::uint8_t {
    NoCommand,
    Processed,
    Unsupported
};

struct RdpImageState final {
    std::uint8_t format = 0;
    std::uint8_t size = 0;
    std::uint16_t width = 0;
    std::uint32_t address = 0;
};

struct RdpTileState final {
    std::uint8_t format = 0;
    std::uint8_t size = 0;
    std::uint16_t line = 0;
    std::uint16_t tmem = 0;
    std::uint8_t palette = 0;
    bool clampT = false;
    bool mirrorT = false;
    std::uint8_t maskT = 0;
    std::uint8_t shiftT = 0;
    bool clampS = false;
    bool mirrorS = false;
    std::uint8_t maskS = 0;
    std::uint8_t shiftS = 0;
};

struct RdpTileSizeState final {
    std::uint16_t sl = 0;
    std::uint16_t tl = 0;
    std::uint16_t sh = 0;
    std::uint16_t th = 0;
};

struct RdpScissorState final {
    std::uint16_t xh = 0;
    std::uint16_t yh = 0;
    std::uint16_t xl = 0;
    std::uint16_t yl = 0;
    bool fieldEnable = false;
    bool oddLine = false;
};

struct RdpCombineCycle final {
    std::uint8_t colorA = 0;
    std::uint8_t colorB = 0;
    std::uint8_t colorC = 0;
    std::uint8_t colorD = 0;
    std::uint8_t alphaA = 0;
    std::uint8_t alphaB = 0;
    std::uint8_t alphaC = 0;
    std::uint8_t alphaD = 0;
};

struct RdpOtherModesState final {
    std::uint8_t cycleType = 0;
    bool perspectiveTexture = false;
    bool detailTexture = false;
    bool sharpenTexture = false;
    bool textureLod = false;
    bool textureLut = false;
    bool textureLutType = false;
    bool sampleType = false;
    bool midTexel = false;
    bool biLerp0 = false;
    bool biLerp1 = false;
    bool convertOne = false;
    bool keyEnable = false;
    std::uint8_t rgbDither = 0;
    std::uint8_t alphaDither = 0;
    std::array<std::uint8_t, 2> blendM1a{};
    std::array<std::uint8_t, 2> blendM1b{};
    std::array<std::uint8_t, 2> blendM2a{};
    std::array<std::uint8_t, 2> blendM2b{};
    bool forceBlend = false;
    bool alphaCoverageSelect = false;
    bool coverageTimesAlpha = false;
    std::uint8_t zMode = 0;
    std::uint8_t coverageDestination = 0;
    bool colorOnCoverage = false;
    bool imageRead = false;
    bool zUpdate = false;
    bool zCompare = false;
    bool antialias = false;
    bool zSourceSelect = false;
    bool ditherAlphaEnable = false;
    bool alphaCompare = false;
};

struct RdpSyncState final {
    std::uint64_t syncLoadCount = 0;
    std::uint64_t syncPipeCount = 0;
    std::uint64_t syncTileCount = 0;
    std::uint64_t syncFullCount = 0;

    // Raw command words are retained alongside decoded state for diagnostics.
    std::uint64_t scissorCommand = 0;
    std::uint64_t otherModesCommand = 0;
    std::uint64_t combineCommand = 0;
    std::uint64_t fillColorCommand = 0;
    std::uint64_t fogColorCommand = 0;
    std::uint64_t blendColorCommand = 0;
    std::uint64_t primColorCommand = 0;
    std::uint64_t envColorCommand = 0;
    std::uint64_t zImageCommand = 0;
    std::uint64_t textureImageCommand = 0;
    std::uint64_t colorImageCommand = 0;
    std::array<std::uint64_t, 8> tileCommands{};
    std::array<std::uint64_t, 8> tileSizeCommands{};
    bool hasScissor = false;
    bool hasOtherModes = false;
    bool hasCombine = false;
    bool hasFillColor = false;
    bool hasFogColor = false;
    bool hasBlendColor = false;
    bool hasPrimColor = false;
    bool hasEnvColor = false;
    bool hasZImage = false;
    bool hasTextureImage = false;
    bool hasColorImage = false;
    std::array<bool, 8> hasTile{};
    std::array<bool, 8> hasTileSize{};

    RdpScissorState scissor{};
    RdpImageState textureImage{};
    RdpImageState colorImage{};
    std::array<RdpTileState, 8> tiles{};
    std::array<RdpTileSizeState, 8> tileSizes{};
    std::uint32_t fillColor = 0;
    std::uint32_t fogColor = 0;
    std::uint32_t blendColor = 0;
    std::uint32_t primColor = 0;
    std::uint32_t envColor = 0;
    std::uint32_t zImageAddress = 0;
    std::uint8_t primLodMinimum = 0;
    std::uint8_t primLodFraction = 0;
    RdpOtherModesState otherModes{};
    std::array<RdpCombineCycle, 2> combine{};
};

class RdpCommandProcessor final {
public:
    // Processes the next command that has a supported non-rendering implementation.
    // Unsupported commands are consumed and reported explicitly; no pixels are rendered.
    static RdpProcessResult processNext(Bus& bus, RdpSyncState& state) noexcept;
};

} // namespace m64plus::n64
