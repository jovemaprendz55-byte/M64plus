#include "n64/rdp.h"

#include "n64/bus.h"

namespace m64plus::n64 {
namespace {
std::int16_t signExtend9(std::uint64_t value) noexcept {
    const std::uint16_t bits = static_cast<std::uint16_t>(value & 0x01FFU);
    const std::int16_t signedValue = static_cast<std::int16_t>(bits);
    return (bits & 0x0100U) != 0U
        ? static_cast<std::int16_t>(signedValue - 0x0200)
        : signedValue;
}
} // namespace


void RdpTextureMemory::reset() noexcept {
    bytes_.fill(0U);
}
bool RdpTextureMemory::readByte(std::uint16_t address, std::uint8_t& value) const noexcept {
    value = bytes_[static_cast<std::size_t>(address) & (kSize - 1U)];
    return true;
}
bool RdpTextureMemory::writeByte(std::uint16_t address, std::uint8_t value) noexcept {
    bytes_[static_cast<std::size_t>(address) & (kSize - 1U)] = value;
    return true;
}
bool RdpTextureMemory::read64(std::uint16_t address, std::uint64_t& value) const noexcept {
    value = 0U;
    for (std::size_t i = 0; i < 8U; ++i) {
        const std::size_t index = (static_cast<std::size_t>(address) + i) & (kSize - 1U);
        value = (value << 8U) | bytes_[index];
    }
    return true;
}
bool RdpTextureMemory::write64(std::uint16_t address, std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < 8U; ++i) {
        const unsigned shift = static_cast<unsigned>((7U - i) * 8U);
        const std::size_t index = (static_cast<std::size_t>(address) + i) & (kSize - 1U);
        bytes_[index] = static_cast<std::uint8_t>((value >> shift) & 0xFFU);
    }
    return true;
}

RdpCommandInfo RdpCommandDecoder::decode(std::uint64_t header) noexcept {
    const auto opcode = static_cast<std::uint8_t>((header >> 56U) & 0x3FU);
    std::uint8_t words = 1U;

    // Triangle commands contain a four-word base followed by optional
    // shade (8), texture (8), and Z (2) coefficient blocks.
    if (opcode >= 0x08U && opcode <= 0x0FU) {
        words = static_cast<std::uint8_t>(
            4U + ((opcode & 0x04U) != 0U ? 8U : 0U) +
            ((opcode & 0x02U) != 0U ? 8U : 0U) +
            ((opcode & 0x01U) != 0U ? 2U : 0U));
    } else if (opcode == 0x24U || opcode == 0x25U) {
        // Texture Rectangle and Texture Rectangle Flip.
        words = 2U;
    }

    // All six-bit opcode values have a defined framing length. This flag
    // distinguishes currently documented command families from reserved
    // encodings; unknown commands still consume their one-word header.
    const bool known =
        (opcode >= 0x08U && opcode <= 0x0FU) ||
        (opcode >= 0x24U && opcode <= 0x3FU);
    return {opcode, words, known};
}

bool RdpCommandStream::fetchNext(Bus& bus, RdpCommandPacket& packet) noexcept {
    std::uint64_t header = 0;
    if (!bus.peekDpCommand(header)) return false;

    const RdpCommandInfo info = RdpCommandDecoder::decode(header);
    if (info.lengthWords == 0U || info.lengthWords > RdpCommandPacket::kMaxWords)
        return false;

    RdpCommandPacket candidate{};
    candidate.info = info;
    if (!bus.readDpCommandWords(candidate.words.data(), info.lengthWords))
        return false;

    packet = candidate;
    return true;
}

RdpProcessResult RdpCommandProcessor::processNext(
    Bus& bus, RdpSyncState& state) noexcept {
    RdpCommandPacket packet{};
    if (!RdpCommandStream::fetchNext(bus, packet))
        return RdpProcessResult::NoCommand;

    switch (packet.info.opcode) {
    case 0x26U: // SYNC_LOAD
        ++state.syncLoadCount;
        return RdpProcessResult::Processed;
    case 0x27U: // SYNC_PIPE
        ++state.syncPipeCount;
        return RdpProcessResult::Processed;
    case 0x28U: // SYNC_TILE
        ++state.syncTileCount;
        return RdpProcessResult::Processed;
    case 0x29U: // SYNC_FULL
        ++state.syncFullCount;
        // MI interrupt source 5 is the Display Processor.
        bus.setMiInterrupt(5U, true);
        return RdpProcessResult::Processed;
    case 0x30U: // LOAD_TLUT
    case 0x33U: // LOAD_BLOCK
    case 0x34U: { // LOAD_TILE
        const std::uint64_t command = packet.words[0];
        const std::uint8_t tile = static_cast<std::uint8_t>((command >> 24U) & 0x07U);
        RdpTextureLoadState load{};
        load.kind = packet.info.opcode == 0x30U ? RdpTextureLoadKind::Tlut
            : (packet.info.opcode == 0x33U ? RdpTextureLoadKind::Block
                                          : RdpTextureLoadKind::Tile);
        load.tile = tile;
        load.sl = static_cast<std::uint16_t>((command >> 44U) & 0x0FFFU);
        load.tl = static_cast<std::uint16_t>((command >> 32U) & 0x0FFFU);
        load.sh = static_cast<std::uint16_t>((command >> 12U) & 0x0FFFU);
        if (packet.info.opcode == 0x33U) {
            load.dxt = static_cast<std::uint16_t>(command & 0x0FFFU);
        } else {
            load.th = static_cast<std::uint16_t>(command & 0x0FFFU);
        }
        load.rawCommand = command;
        state.lastTextureLoad = load;
        state.hasTextureLoad = true;
        return RdpProcessResult::Processed;
    }
    case 0x2AU: { // SET_KEY_GB
        const std::uint64_t command = packet.words[0];
        state.keyGbCommand = command;
        state.keyGb = {
            static_cast<std::uint16_t>((command >> 44U) & 0x0FFFU),
            static_cast<std::uint16_t>((command >> 32U) & 0x0FFFU),
            static_cast<std::uint8_t>((command >> 24U) & 0xFFU),
            static_cast<std::uint8_t>((command >> 16U) & 0xFFU),
            static_cast<std::uint8_t>((command >> 8U) & 0xFFU),
            static_cast<std::uint8_t>(command & 0xFFU)
        };
        state.hasKeyGb = true;
        return RdpProcessResult::Processed;
    }
    case 0x2BU: { // SET_KEY_R
        const std::uint64_t command = packet.words[0];
        state.keyRCommand = command;
        state.keyR = {
            static_cast<std::uint16_t>((command >> 16U) & 0x0FFFU),
            static_cast<std::uint8_t>((command >> 8U) & 0xFFU),
            static_cast<std::uint8_t>(command & 0xFFU)
        };
        state.hasKeyR = true;
        return RdpProcessResult::Processed;
    }
    case 0x2CU: { // SET_CONVERT
        const std::uint64_t command = packet.words[0];
        state.convertCommand = command;
        state.convert.coefficients = {{
            signExtend9(command >> 45U), signExtend9(command >> 36U),
            signExtend9(command >> 27U), signExtend9(command >> 18U),
            signExtend9(command >> 9U), signExtend9(command)
        }};
        state.hasConvert = true;
        return RdpProcessResult::Processed;
    }
    case 0x2EU: { // SET_PRIM_DEPTH
        const std::uint64_t command = packet.words[0];
        state.primDepthCommand = command;
        state.primDepth = static_cast<std::uint16_t>((command >> 16U) & 0xFFFFU);
        state.primDeltaZ = static_cast<std::uint16_t>(command & 0xFFFFU);
        state.hasPrimDepth = true;
        return RdpProcessResult::Processed;
    }
    case 0x2DU: { // SET_SCISSOR
        const std::uint64_t command = packet.words[0];
        state.scissorCommand = command;
        state.scissor = {
            static_cast<std::uint16_t>((command >> 44U) & 0x0FFFU),
            static_cast<std::uint16_t>((command >> 32U) & 0x0FFFU),
            static_cast<std::uint16_t>((command >> 12U) & 0x0FFFU),
            static_cast<std::uint16_t>(command & 0x0FFFU),
            ((command >> 25U) & 1U) != 0U,
            ((command >> 24U) & 1U) != 0U
        };
        state.hasScissor = true;
        return RdpProcessResult::Processed;
    }
    case 0x2FU: { // SET_OTHER_MODES
        const std::uint64_t command = packet.words[0];
        state.otherModesCommand = command;
        state.otherModes = {
            static_cast<std::uint8_t>((command >> 52U) & 0x03U),
            ((command >> 51U) & 1U) != 0U,
            ((command >> 50U) & 1U) != 0U,
            ((command >> 49U) & 1U) != 0U,
            ((command >> 48U) & 1U) != 0U,
            ((command >> 47U) & 1U) != 0U,
            ((command >> 46U) & 1U) != 0U,
            ((command >> 45U) & 1U) != 0U,
            ((command >> 44U) & 1U) != 0U,
            ((command >> 43U) & 1U) != 0U,
            ((command >> 42U) & 1U) != 0U,
            ((command >> 41U) & 1U) != 0U,
            ((command >> 40U) & 1U) != 0U,
            static_cast<std::uint8_t>((command >> 38U) & 0x03U),
            static_cast<std::uint8_t>((command >> 36U) & 0x03U),
            {{static_cast<std::uint8_t>((command >> 30U) & 0x03U),
              static_cast<std::uint8_t>((command >> 28U) & 0x03U)}},
            {{static_cast<std::uint8_t>((command >> 26U) & 0x03U),
              static_cast<std::uint8_t>((command >> 24U) & 0x03U)}},
            {{static_cast<std::uint8_t>((command >> 22U) & 0x03U),
              static_cast<std::uint8_t>((command >> 20U) & 0x03U)}},
            {{static_cast<std::uint8_t>((command >> 18U) & 0x03U),
              static_cast<std::uint8_t>((command >> 16U) & 0x03U)}},
            ((command >> 14U) & 1U) != 0U,
            ((command >> 13U) & 1U) != 0U,
            ((command >> 12U) & 1U) != 0U,
            static_cast<std::uint8_t>((command >> 10U) & 0x03U),
            static_cast<std::uint8_t>((command >> 8U) & 0x03U),
            ((command >> 7U) & 1U) != 0U,
            ((command >> 6U) & 1U) != 0U,
            ((command >> 5U) & 1U) != 0U,
            ((command >> 4U) & 1U) != 0U,
            ((command >> 3U) & 1U) != 0U,
            ((command >> 2U) & 1U) != 0U,
            ((command >> 1U) & 1U) != 0U,
            (command & 1U) != 0U
        };
        state.hasOtherModes = true;
        return RdpProcessResult::Processed;
    }
    case 0x3CU: { // SET_COMBINE
        const std::uint64_t command = packet.words[0];
        state.combineCommand = command;
        state.combine[0] = {
            static_cast<std::uint8_t>((command >> 52U) & 0x0FU),
            static_cast<std::uint8_t>((command >> 28U) & 0x0FU),
            static_cast<std::uint8_t>((command >> 47U) & 0x1FU),
            static_cast<std::uint8_t>((command >> 15U) & 0x07U),
            static_cast<std::uint8_t>((command >> 44U) & 0x07U),
            static_cast<std::uint8_t>((command >> 12U) & 0x07U),
            static_cast<std::uint8_t>((command >> 41U) & 0x07U),
            static_cast<std::uint8_t>((command >> 9U) & 0x07U)
        };
        state.combine[1] = {
            static_cast<std::uint8_t>((command >> 37U) & 0x0FU),
            static_cast<std::uint8_t>((command >> 24U) & 0x0FU),
            static_cast<std::uint8_t>((command >> 32U) & 0x1FU),
            static_cast<std::uint8_t>((command >> 6U) & 0x07U),
            static_cast<std::uint8_t>((command >> 21U) & 0x07U),
            static_cast<std::uint8_t>((command >> 3U) & 0x07U),
            static_cast<std::uint8_t>((command >> 18U) & 0x07U),
            static_cast<std::uint8_t>(command & 0x07U)
        };
        state.hasCombine = true;
        return RdpProcessResult::Processed;
    }
    case 0x32U: { // SET_TILE_SIZE
        const std::uint64_t command = packet.words[0];
        const std::size_t tile = static_cast<std::size_t>((command >> 24U) & 0x07U);
        state.tileSizeCommands[tile] = command;
        state.tileSizes[tile] = {
            static_cast<std::uint16_t>((command >> 44U) & 0x0FFFU),
            static_cast<std::uint16_t>((command >> 32U) & 0x0FFFU),
            static_cast<std::uint16_t>((command >> 12U) & 0x0FFFU),
            static_cast<std::uint16_t>(command & 0x0FFFU)
        };
        state.hasTileSize[tile] = true;
        return RdpProcessResult::Processed;
    }
    case 0x35U: { // SET_TILE
        const std::uint64_t command = packet.words[0];
        const std::size_t tile = static_cast<std::size_t>((command >> 24U) & 0x07U);
        state.tileCommands[tile] = command;
        state.tiles[tile] = {
            static_cast<std::uint8_t>((command >> 53U) & 0x07U),
            static_cast<std::uint8_t>((command >> 51U) & 0x03U),
            static_cast<std::uint16_t>((command >> 41U) & 0x01FFU),
            static_cast<std::uint16_t>((command >> 32U) & 0x01FFU),
            static_cast<std::uint8_t>((command >> 20U) & 0x0FU),
            ((command >> 19U) & 1U) != 0U,
            ((command >> 18U) & 1U) != 0U,
            static_cast<std::uint8_t>((command >> 14U) & 0x0FU),
            static_cast<std::uint8_t>((command >> 10U) & 0x0FU),
            ((command >> 9U) & 1U) != 0U,
            ((command >> 8U) & 1U) != 0U,
            static_cast<std::uint8_t>((command >> 4U) & 0x0FU),
            static_cast<std::uint8_t>(command & 0x0FU)
        };
        state.hasTile[tile] = true;
        return RdpProcessResult::Processed;
    }
    case 0x37U: // SET_FILL_COLOR
    case 0x38U: // SET_FOG_COLOR
    case 0x39U: // SET_BLEND_COLOR
    case 0x3AU: // SET_PRIM_COLOR
    case 0x3BU: { // SET_ENV_COLOR
        const std::uint64_t command = packet.words[0];
        const std::uint32_t color = static_cast<std::uint32_t>(command & 0xFFFFFFFFU);
        switch (packet.info.opcode) {
        case 0x37U:
            state.fillColorCommand = command;
            state.fillColor = color;
            state.hasFillColor = true;
            break;
        case 0x38U:
            state.fogColorCommand = command;
            state.fogColor = color;
            state.hasFogColor = true;
            break;
        case 0x39U:
            state.blendColorCommand = command;
            state.blendColor = color;
            state.hasBlendColor = true;
            break;
        case 0x3AU:
            state.primColorCommand = command;
            state.primColor = color;
            state.primLodMinimum = static_cast<std::uint8_t>((command >> 40U) & 0x1FU);
            state.primLodFraction = static_cast<std::uint8_t>((command >> 32U) & 0xFFU);
            state.hasPrimColor = true;
            break;
        default:
            state.envColorCommand = command;
            state.envColor = color;
            state.hasEnvColor = true;
            break;
        }
        return RdpProcessResult::Processed;
    }
    case 0x3EU: // SET_Z_IMAGE
        state.zImageCommand = packet.words[0];
        state.zImageAddress = static_cast<std::uint32_t>(packet.words[0] & 0x03FFFFFFU);
        state.hasZImage = true;
        return RdpProcessResult::Processed;
    case 0x3DU: // SET_TEXTURE_IMAGE
    case 0x3FU: { // SET_COLOR_IMAGE
        const std::uint64_t command = packet.words[0];
        const RdpImageState image{
            static_cast<std::uint8_t>((command >> 53U) & 0x07U),
            static_cast<std::uint8_t>((command >> 51U) & 0x03U),
            static_cast<std::uint16_t>(((command >> 32U) & 0x03FFU) + 1U),
            static_cast<std::uint32_t>(command & 0x03FFFFFFU)
        };
        if (packet.info.opcode == 0x3DU) {
            state.textureImageCommand = command;
            state.textureImage = image;
            state.hasTextureImage = true;
        } else {
            state.colorImageCommand = command;
            state.colorImage = image;
            state.hasColorImage = true;
        }
        return RdpProcessResult::Processed;
    }
    default:
        return RdpProcessResult::Unsupported;
    }
}

} // namespace m64plus::n64
