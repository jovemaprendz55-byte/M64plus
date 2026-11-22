#include "n64/rdp.h"

namespace m64plus::n64 {

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

} // namespace m64plus::n64
