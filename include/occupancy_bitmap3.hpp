#pragma once

#include <array>
#include <bit>
#include <cstdint>

namespace matching {

inline constexpr uint32_t kMaxInBandLevels = 64U * 64U * 64U;

// Three-level 64-ary occupancy index for up to 262,144 price ticks.
// All indices must be less than kMaxInBandLevels.
class OccupancyBitmap3 final {
public:
  void set(uint32_t index) noexcept {
    const uint32_t leaf_word = index >> 6;
    const uint32_t leaf_bit = index & 63U;
    const uint64_t old_leaf = leaves_[leaf_word];
    leaves_[leaf_word] = old_leaf | (uint64_t{1} << leaf_bit);
    if (old_leaf != 0) {
      return;
    }

    const uint32_t middle_word = leaf_word >> 6;
    const uint32_t middle_bit = leaf_word & 63U;
    const uint64_t old_middle = middle_[middle_word];
    middle_[middle_word] = old_middle | (uint64_t{1} << middle_bit);
    if (old_middle == 0) {
      root_ |= uint64_t{1} << middle_word;
    }
  }

  void clear(uint32_t index) noexcept {
    const uint32_t leaf_word = index >> 6;
    const uint32_t leaf_bit = index & 63U;
    leaves_[leaf_word] &= ~(uint64_t{1} << leaf_bit);
    if (leaves_[leaf_word] != 0) {
      return;
    }

    const uint32_t middle_word = leaf_word >> 6;
    const uint32_t middle_bit = leaf_word & 63U;
    middle_[middle_word] &= ~(uint64_t{1} << middle_bit);
    if (middle_[middle_word] == 0) {
      root_ &= ~(uint64_t{1} << middle_word);
    }
  }

  bool empty() const noexcept { return root_ == 0; }

  uint32_t lowest() const noexcept {
    const uint32_t middle_word = static_cast<uint32_t>(std::countr_zero(root_));
    const uint32_t leaf_word =
        (middle_word << 6) +
        static_cast<uint32_t>(std::countr_zero(middle_[middle_word]));
    return (leaf_word << 6) +
           static_cast<uint32_t>(std::countr_zero(leaves_[leaf_word]));
  }

  uint32_t highest() const noexcept {
    const uint32_t middle_word =
        63U - static_cast<uint32_t>(std::countl_zero(root_));
    const uint32_t leaf_word =
        (middle_word << 6) + 63U -
        static_cast<uint32_t>(std::countl_zero(middle_[middle_word]));
    return (leaf_word << 6) + 63U -
           static_cast<uint32_t>(std::countl_zero(leaves_[leaf_word]));
  }

private:
  std::array<uint64_t, 4096> leaves_{};
  std::array<uint64_t, 64> middle_{};
  uint64_t root_ = 0;
};

} // namespace matching
