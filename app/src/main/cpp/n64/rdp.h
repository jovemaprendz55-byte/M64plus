#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace m64plus::n64 {

class Bus;

struct RdpCommandInfo final {
    std::uint8_t opcode = 0;
    std::uint8_t lengthWords = 1;
    bool knownOpcode = false;
};

// Decodes the command header only. It does not execute rendering operations.
struct RdpTriangleLayout;
struct RdpTriangleEdgeState;
struct RdpCommandPacket;
struct RdpTriangleCommand;
struct RdpTextureRectangleCommand;

class RdpCommandDecoder final {
public:
    static RdpCommandInfo decode(std::uint64_t header) noexcept;
    static bool decodeTriangleLayout(std::uint8_t opcode, RdpTriangleLayout& layout) noexcept;
    static bool decodeTriangleEdge(const std::array<std::uint64_t, 4>& words,
                                   RdpTriangleEdgeState& edge) noexcept;
    static bool decodeTriangleCommand(const RdpCommandPacket& packet,
                                      RdpTriangleCommand& command) noexcept;
    static bool decodeTextureRectangle(const RdpCommandPacket& packet,
                                       RdpTextureRectangleCommand& command) noexcept;
};

// Raw texture-rectangle fields. Coordinates are retained in 10.2 form;
// S/T and their derivatives retain their signed 16-bit command representation.
struct RdpTextureRectangleCommand final {
    std::uint16_t xh = 0;
    std::uint16_t yh = 0;
    std::uint16_t xl = 0;
    std::uint16_t yl = 0;
    std::uint8_t tile = 0;
    std::int16_t s = 0;
    std::int16_t t = 0;
    std::int16_t dsdx = 0;
    std::int16_t dtdy = 0;
    bool flip = false;
    std::array<std::uint64_t, 2> rawWords{};
};

struct RdpTriangleEdgeState final {
    bool leftMajor = false;
    // RDP texture LOD level (mip levels minus one) and tile descriptor index.
    std::uint8_t level = 0;
    std::uint8_t tile = 0;
    std::uint16_t yLow = 0;
    std::uint16_t yMiddle = 0;
    std::uint16_t yHigh = 0;
    std::int32_t xLeft = 0;
    std::int32_t dxLeftDy = 0;
    std::int32_t xHigh = 0;
    std::int32_t dxHighDy = 0;
    std::int32_t xMiddle = 0;
    std::int32_t dxMiddleDy = 0;
};

struct RdpTriangleLayout final {
    std::uint8_t wordCount = 0;
    std::uint8_t shadeWordOffset = 0;
    std::uint8_t textureWordOffset = 0;
    std::uint8_t zWordOffset = 0;
    bool hasShade = false;
    bool hasTexture = false;
    bool hasZ = false;
};

// Shade block order: RGBA, dRGBA/dX, RGBA fraction, dRGBA/dX fraction,
// dRGBA/dE, dRGBA/dY, dRGBA/dE fraction, dRGBA/dY fraction.
struct RdpTriangleShadeCoefficients final {
    std::array<std::int16_t, 4> rgba{};
    std::array<std::int16_t, 4> drgbaDx{};
    std::array<std::int16_t, 4> rgbaFraction{};
    std::array<std::int16_t, 4> drgbaDxFraction{};
    std::array<std::int16_t, 4> drgbaDe{};
    std::array<std::int16_t, 4> drgbaDy{};
    std::array<std::int16_t, 4> drgbaDeFraction{};
    std::array<std::int16_t, 4> drgbaDyFraction{};
    // Combined signed fixed-point values: base uses s9.16; deltas use s15.16.
    std::array<std::int32_t, 4> rgbaFixed{};
    std::array<std::int32_t, 4> drgbaDxFixed{};
    std::array<std::int32_t, 4> drgbaDeFixed{};
    std::array<std::int32_t, 4> drgbaDyFixed{};
};

// Texture block order: STW, dSTW/dX, STW fraction, dSTW/dX fraction,
// dSTW/dE, dSTW/dY, dSTW/dE fraction, dSTW/dY fraction.
// Each source word's low 16 bits are not coefficient fields.
struct RdpTriangleTextureCoefficients final {
    std::array<std::int16_t, 3> stw{};
    std::array<std::int16_t, 3> dstwDx{};
    std::array<std::int16_t, 3> stwFraction{};
    std::array<std::int16_t, 3> dstwDxFraction{};
    std::array<std::int16_t, 3> dstwDe{};
    std::array<std::int16_t, 3> dstwDy{};
    std::array<std::int16_t, 3> dstwDeFraction{};
    std::array<std::int16_t, 3> dstwDyFraction{};
};

// Z block order: Z, Z fraction, dZ/dX, dZ/dX fraction,
// dZ/dE, dZ/dE fraction, dZ/dY, dZ/dY fraction.
struct RdpTriangleZCoefficients final {
    std::int16_t z = 0;
    std::int16_t zFraction = 0;
    std::int16_t dzDx = 0;
    std::int16_t dzDxFraction = 0;
    std::int16_t dzDe = 0;
    std::int16_t dzDeFraction = 0;
    std::int16_t dzDy = 0;
    std::int16_t dzDyFraction = 0;
};

struct RdpTriangleCommand final {
    RdpTriangleEdgeState edge{};
    RdpTriangleLayout layout{};
    std::array<std::uint64_t, 8> shadeWords{};
    std::array<std::uint64_t, 8> textureWords{};
    std::array<std::uint64_t, 2> zWords{};
    // Coefficients are signed 16-bit fields in each 64-bit payload word.
    // Shade uses four fields/word; texture uses three (low 16 bits unused);
    // Z uses four fields/word.
    std::array<std::int16_t, 32> shadeCoefficients{};
    std::array<std::int16_t, 24> textureCoefficients{};
    std::array<std::int16_t, 8> zCoefficients{};
    RdpTriangleShadeCoefficients shade{};
    RdpTriangleTextureCoefficients texture{};
    RdpTriangleZCoefficients depth{};
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

struct RdpConvertState final {
    std::array<std::int16_t, 6> coefficients{};
};

struct RdpKeyRState final {
    std::uint16_t width = 0;
    std::uint8_t center = 0;
    std::uint8_t scale = 0;
};

struct RdpKeyGbState final {
    std::uint16_t widthG = 0;
    std::uint16_t widthB = 0;
    std::uint8_t centerG = 0;
    std::uint8_t scaleG = 0;
    std::uint8_t centerB = 0;
    std::uint8_t scaleB = 0;
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

// 4 KiB of RDP on-chip texture memory. Addresses wrap on the 12-bit TMEM bus.
class RdpTextureMemory final {
public:
    static constexpr std::size_t kSize = 4096U;
    void reset() noexcept;
    bool readByte(std::uint16_t address, std::uint8_t& value) const noexcept;
    bool writeByte(std::uint16_t address, std::uint8_t value) noexcept;
    bool read64(std::uint16_t address, std::uint64_t& value) const noexcept;
    bool write64(std::uint16_t address, std::uint64_t value) noexcept;
private:
    std::array<std::uint8_t, kSize> bytes_{};
};

enum class RdpTextureLoadKind : std::uint8_t { None, Tile, Block, Tlut };

struct RdpTextureLoadState final {
    RdpTextureLoadKind kind = RdpTextureLoadKind::None;
    std::uint8_t tile = 0;
    std::uint16_t sl = 0;
    std::uint16_t tl = 0;
    std::uint16_t sh = 0;
    std::uint16_t th = 0;
    std::uint16_t dxt = 0;
    std::uint64_t rawCommand = 0;
};

struct RdpSyncState final {
    RdpTriangleCommand lastTriangle{};
    bool hasTriangle = false;
    RdpTextureRectangleCommand lastTextureRectangle{};
    bool hasTextureRectangle = false;
    RdpTextureLoadState lastTextureLoad{};
    bool hasTextureLoad = false;
    RdpTextureMemory tmem{};
    std::uint64_t syncLoadCount = 0;
    std::uint64_t syncPipeCount = 0;
    std::uint64_t syncTileCount = 0;
    std::uint64_t syncFullCount = 0;

    // Raw command words are retained alongside decoded state for diagnostics.
    std::uint64_t scissorCommand = 0;
    std::uint64_t convertCommand = 0;
    std::uint64_t keyRCommand = 0;
    std::uint64_t keyGbCommand = 0;
    std::uint64_t primDepthCommand = 0;
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
    bool hasConvert = false;
    bool hasKeyR = false;
    bool hasKeyGb = false;
    bool hasPrimDepth = false;
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
    RdpConvertState convert{};
    RdpKeyRState keyR{};
    RdpKeyGbState keyGb{};
    std::uint16_t primDepth = 0;
    std::uint16_t primDeltaZ = 0;
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
