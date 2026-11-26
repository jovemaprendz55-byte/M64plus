#include "n64/rdram.h"
#include <algorithm>
namespace m64plus::n64 {
Rdram::Rdram() : bytes_(kSize, 0), reservationEpochs_(kSize / 16U, 0) {}
void Rdram::reset(std::uint8_t value) {
    std::fill(bytes_.begin(), bytes_.end(), value);
    ++writeEpoch_;
    std::fill(reservationEpochs_.begin(), reservationEpochs_.end(), writeEpoch_);
}
void Rdram::markWrite(std::uint32_t address, std::size_t width) noexcept {
    ++writeEpoch_;
    const std::size_t first = static_cast<std::size_t>(address) / 16U;
    const std::size_t last = (static_cast<std::size_t>(address) + width - 1U) / 16U;
    for (std::size_t line = first; line <= last; ++line)
        reservationEpochs_[line] = writeEpoch_;
}
bool Rdram::contains(std::uint32_t address, std::size_t width) const noexcept {
    const auto offset = static_cast<std::size_t>(address);
    return width <= bytes_.size() && offset <= bytes_.size() - width;
}
bool Rdram::read8(std::uint32_t a, std::uint8_t& v) const {
    if (!contains(a, 1)) return false;
    v = bytes_[a];
    return true;
}
bool Rdram::read16(std::uint32_t a, std::uint16_t& v) const {
    if (!contains(a, 2)) return false;
    v = (static_cast<std::uint16_t>(bytes_[a]) << 8U) | bytes_[a + 1U]; return true;
}
bool Rdram::read32(std::uint32_t a, std::uint32_t& v) const {
    if (!contains(a, 4)) return false;
    v = (static_cast<std::uint32_t>(bytes_[a]) << 24U)
      | (static_cast<std::uint32_t>(bytes_[a + 1U]) << 16U)
      | (static_cast<std::uint32_t>(bytes_[a + 2U]) << 8U)
      | static_cast<std::uint32_t>(bytes_[a + 3U]); return true;
}
bool Rdram::read64(std::uint32_t a, std::uint64_t& v) const {
    if (!contains(a, 8)) return false;
    std::uint32_t hi = 0, lo = 0;
    if (!read32(a, hi) || !read32(a + 4U, lo)) return false;
    v = (static_cast<std::uint64_t>(hi) << 32U) | lo; return true;
}
bool Rdram::write8(std::uint32_t a, std::uint8_t v) {
    if (!contains(a, 1)) return false;
    bytes_[a] = v;
    markWrite(a, 1U);
    return true;
}
bool Rdram::write16(std::uint32_t a, std::uint16_t v) {
    if (!contains(a, 2)) return false;
    bytes_[a] = static_cast<std::uint8_t>(v >> 8U);
    bytes_[a + 1U] = static_cast<std::uint8_t>(v);
    markWrite(a, 2U);
    return true;
}
bool Rdram::write32(std::uint32_t a, std::uint32_t v) {
    if (!contains(a, 4)) return false;
    bytes_[a] = static_cast<std::uint8_t>(v >> 24U);
    bytes_[a + 1U] = static_cast<std::uint8_t>(v >> 16U);
    bytes_[a + 2U] = static_cast<std::uint8_t>(v >> 8U);
    bytes_[a + 3U] = static_cast<std::uint8_t>(v);
    markWrite(a, 4U);
    return true;
}
bool Rdram::write64(std::uint32_t a, std::uint64_t v) {
    if (!contains(a, 8)) return false;
    return write32(a, static_cast<std::uint32_t>(v >> 32U))
        && write32(a + 4U, static_cast<std::uint32_t>(v));
}
const std::uint8_t* Rdram::data() const noexcept { return bytes_.data(); }
std::size_t Rdram::size() const noexcept { return bytes_.size(); }
std::uint64_t Rdram::writeEpoch() const noexcept { return writeEpoch_; }
std::uint64_t Rdram::reservationEpoch(std::uint32_t address) const noexcept {
    return contains(address, 1U) ? reservationEpochs_[static_cast<std::size_t>(address) / 16U] : 0U;
}
} // namespace m64plus::n64
