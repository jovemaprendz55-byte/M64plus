#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
namespace m64plus::n64 {
class Rdram final {
public:
    static constexpr std::size_t kSize = 8U * 1024U * 1024U;
    Rdram();
    void reset(std::uint8_t value = 0);
    bool read8(std::uint32_t address, std::uint8_t& value) const;
    bool read16(std::uint32_t address, std::uint16_t& value) const;
    bool read32(std::uint32_t address, std::uint32_t& value) const;
    bool read64(std::uint32_t address, std::uint64_t& value) const;
    bool write8(std::uint32_t address, std::uint8_t value);
    bool write16(std::uint32_t address, std::uint16_t value);
    bool write32(std::uint32_t address, std::uint32_t value);
    bool write64(std::uint32_t address, std::uint64_t value);
    const std::uint8_t* data() const noexcept;
    std::size_t size() const noexcept;
private:
    bool contains(std::uint32_t address, std::size_t width) const noexcept;
    std::vector<std::uint8_t> bytes_;
};
} // namespace m64plus::n64
