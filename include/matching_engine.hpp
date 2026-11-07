#pragma once

#include "event_sink.hpp"
#include "matching_types.hpp"
#include "occupancy_bitmap3.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace matching {

inline constexpr uint32_t kInvalid = std::numeric_limits<uint32_t>::max();

// Mapping from order ID to the index of the order in the orders_ vector.
// A better design would be returning the order ID as a handle to the caller,
// and require the caller to use the handle when calling cancel order,
// but the assignment specifies that the order ID is supplied by the caller.
// Another optimization would be a fixed-capacity, open-addressed hashmap
// without per-order allocations. std::unordered_map is used for simplicity.
using OrderIdIndex = std::unordered_map<uint64_t, uint32_t>;

// Matching engine for a single symbol.
// Prices are signed integer units (e.g. 1 unit = $0.000001).
// kTickSizeUnits is the minimum price increment in units (e.g. 50'000).
// kLuldBps is the LULD band width in basis points (1/100 of a percent).
// reference_price_units is used to compute the LULD band.
// In-band orders can match, out-of-band orders can only rest or be canceled.
// Events are emitted through the EventSink supplied at construction.
template <int64_t kTickSizeUnits, uint32_t kLuldBps>
class MatchingEngine final {
  static_assert(kTickSizeUnits > 0, "Tick size must be positive");
  static_assert(kLuldBps < 10'000, "LULD must be less than 100 percent");

public:
  explicit MatchingEngine(int64_t reference_price_units, size_t max_live_orders,
                          EventSink& event_sink)
      : band_(make_band(reference_price_units)), bids_(band_.level_count),
        asks_(band_.level_count), orders_(validated_capacity(max_live_orders)),
        event_sink_(event_sink) {
    id_index_.max_load_factor(0.7F);
    id_index_.reserve(max_live_orders);
  }

  MatchingEngine(const MatchingEngine&) = delete;
  MatchingEngine& operator=(const MatchingEngine&) = delete;
  MatchingEngine(MatchingEngine&&) = delete;
  MatchingEngine& operator=(MatchingEngine&&) = delete;

  [[nodiscard]] Status add_order(uint64_t order_id, Side side,
                                 uint64_t quantity, int64_t price_units) {
    if (order_id == 0) {
      return Status::InvalidOrderId;
    }
    if (side != Side::Buy && side != Side::Sell) {
      return Status::InvalidSide;
    }
    if (quantity == 0) {
      return Status::InvalidQuantity;
    }
    if (price_units <= 0 || price_units % kTickSizeUnits != 0) {
      return Status::InvalidPrice;
    }
    if (id_index_.contains(order_id)) {
      return Status::DuplicateActiveOrderId;
    }

    const int64_t price_tick = price_units / kTickSizeUnits;
    if (!is_in_band(price_tick)) {
      return insert_resting(order_id, side, quantity, price_tick);
    }

    if (side == Side::Buy) {
      return add_in_band<Side::Buy>(order_id, quantity, price_tick);
    } else {
      return add_in_band<Side::Sell>(order_id, quantity, price_tick);
    }
  }

  [[nodiscard]] Status cancel_order(uint64_t order_id) {
    if (order_id == 0) {
      return Status::InvalidOrderId;
    }
    const auto it = id_index_.find(order_id);
    if (it == id_index_.end()) {
      return Status::UnknownOrderId;
    }
    remove_resting(it->second);
    return Status::Ok;
  }

  std::optional<int64_t> best_price_units(Side side) const noexcept {
    if (side != Side::Buy && side != Side::Sell) {
      return std::nullopt;
    }
    const BookSide& book_side = side == Side::Buy ? bids_ : asks_;
    if (book_side.best_index == kInvalid) {
      return std::nullopt;
    }
    const int64_t price_tick =
        band_.lower_tick + static_cast<int64_t>(book_side.best_index);
    return price_tick * kTickSizeUnits;
  }

  std::optional<uint64_t> resting_quantity(uint64_t order_id) const noexcept {
    const auto it = id_index_.find(order_id);
    if (it == id_index_.end()) {
      return std::nullopt;
    }
    return orders_[it->second].quantity;
  }

  size_t active_order_count() const noexcept { return active_order_count_; }
  int64_t lower_price_tick() const noexcept { return band_.lower_tick; }
  int64_t upper_price_tick() const noexcept { return band_.upper_tick; }

private:
  struct PriceLevel {
    uint32_t head = kInvalid;
    uint32_t tail = kInvalid;
    uint32_t count = 0;
  };

  struct BookSide {
    explicit BookSide(size_t level_count) : levels(level_count) {}

    // directly indexed price levels within the LULD band
    std::vector<PriceLevel> levels;

    // three-level bitmap tracking occupied in-band price levels
    OccupancyBitmap3 occupied;

    // index of the best in-band price level, or kInvalid if empty
    uint32_t best_index = kInvalid;

    // price levels outside LULD
    std::map<int64_t, PriceLevel> outside;
  };

  struct OrderNode {
    uint64_t id = 0;
    uint64_t quantity = 0;
    int64_t price_tick = 0;
    PriceLevel* level = nullptr; // Non-owning, stable for the order's lifetime.
    uint32_t prev = kInvalid;
    uint32_t next = kInvalid;
    Side side = Side::Buy;
  };

  struct PriceBand {
    int64_t lower_tick;
    int64_t upper_tick;
    size_t level_count;
  };

  bool is_in_band(int64_t price_tick) const noexcept {
    return price_tick >= band_.lower_tick && price_tick <= band_.upper_tick;
  }

  uint32_t band_index(int64_t price_tick) const noexcept {
    return static_cast<uint32_t>(price_tick - band_.lower_tick);
  }

  bool has_free_slot() const noexcept {
    return free_head_ != kInvalid ||
           static_cast<size_t>(next_unused_) < orders_.size();
  }

  uint32_t acquire_slot() noexcept {
    if (free_head_ != kInvalid) {
      const uint32_t handle = free_head_;
      free_head_ = orders_[handle].next;
      return handle;
    }
    return next_unused_++;
  }

  void release_slot(uint32_t handle) noexcept {
    orders_[handle].next = free_head_;
    free_head_ = handle;
  }

  static void append_to_level(PriceLevel& level, std::vector<OrderNode>& nodes,
                              uint32_t handle) noexcept {
    OrderNode& node = nodes[handle];
    node.prev = level.tail;
    node.next = kInvalid;
    if (level.tail != kInvalid) {
      nodes[level.tail].next = handle;
    } else {
      level.head = handle;
    }
    level.tail = handle;
    ++level.count;
  }

  void activate_price(BookSide& book_side, Side side, uint32_t index) noexcept {
    book_side.occupied.set(index);
    if (book_side.best_index == kInvalid ||
        (side == Side::Buy && index > book_side.best_index) ||
        (side == Side::Sell && index < book_side.best_index)) {
      book_side.best_index = index;
    }
  }

  void deactivate_price(BookSide& book_side, Side side,
                        uint32_t index) noexcept {
    book_side.occupied.clear(index);
    if (book_side.best_index == index) {
      book_side.best_index = book_side.occupied.empty() ? kInvalid
                             : side == Side::Buy ? book_side.occupied.highest()
                                                 : book_side.occupied.lowest();
    }
  }

  template <Side incoming_side>
  Status add_in_band(uint64_t order_id, uint64_t quantity, int64_t price_tick) {
    BookSide& opposite = incoming_side == Side::Buy ? asks_ : bids_;
    const auto crosses = [&]() noexcept {
      if (opposite.best_index == kInvalid) {
        return false;
      }
      const int64_t best_tick =
          band_.lower_tick + static_cast<int64_t>(opposite.best_index);
      if constexpr (incoming_side == Side::Buy) {
        return price_tick >= best_tick;
      } else {
        return price_tick <= best_tick;
      }
    };

    if (!crosses()) {
      return insert_resting(order_id, incoming_side, quantity, price_tick);
    }

    while (quantity != 0 && crosses()) {
      const uint32_t best_index = opposite.best_index;
      PriceLevel& level = opposite.levels[best_index];
      const uint32_t maker_handle = level.head;
      OrderNode& maker = orders_[maker_handle];

      const uint64_t traded_quantity = std::min(quantity, maker.quantity);
      quantity -= traded_quantity;
      maker.quantity -= traded_quantity;

      const uint64_t maker_id = maker.id;
      const uint64_t maker_remaining = maker.quantity;
      const int64_t trade_price_units = maker.price_tick * kTickSizeUnits;
      if (maker_remaining == 0) {
        remove_resting(maker_handle);
      }

      event_sink_.emit(
          Event{EventKind::Trade, 0, traded_quantity, trade_price_units});
      event_sink_.emit(fill_event(order_id, quantity));
      event_sink_.emit(fill_event(maker_id, maker_remaining));
    }

    if (quantity != 0) {
      const Status insert_status =
          insert_resting(order_id, incoming_side, quantity, price_tick);
      if (insert_status != Status::Ok) {
        return insert_status;
      }
    }
    return event_sink_.status();
  }

  Status insert_resting(uint64_t order_id, Side side, uint64_t quantity,
                        int64_t price_tick) {
    if (!has_free_slot()) {
      return Status::OrderCapacityExceeded;
    }

    BookSide& book_side = side == Side::Buy ? bids_ : asks_;
    const bool inside = is_in_band(price_tick);
    const uint32_t index = inside ? band_index(price_tick) : 0;
    PriceLevel* level = nullptr;
    bool inserted_outside_level = false;
    const uint32_t handle = acquire_slot();

    // Allocate/index before linking the order, so allocation failures cannot
    // leave a partially inserted FIFO node or inconsistent occupancy bitmap.
    try {
      if (inside) {
        level = &book_side.levels[index];
      } else {
        const auto [it, inserted] = book_side.outside.try_emplace(price_tick);
        level = &it->second;
        inserted_outside_level = inserted;
      }
      const bool inserted = id_index_.try_emplace(order_id, handle).second;
      if (!inserted) {
        if (inserted_outside_level) {
          book_side.outside.erase(price_tick);
        }
        release_slot(handle);
        return Status::DuplicateActiveOrderId;
      }
    } catch (const std::bad_alloc&) {
      if (inserted_outside_level) {
        book_side.outside.erase(price_tick);
      }
      release_slot(handle);
      return Status::AllocationFailed;
    }

    const bool first_at_price = level->count == 0;
    orders_[handle] = OrderNode{.id = order_id,
                                .quantity = quantity,
                                .price_tick = price_tick,
                                .level = level,
                                .prev = kInvalid,
                                .next = kInvalid,
                                .side = side};
    append_to_level(*level, orders_, handle);
    ++active_order_count_;

    if (inside && first_at_price) {
      activate_price(book_side, side, index);
    }
    return Status::Ok;
  }

  void remove_resting(uint32_t handle) {
    const OrderNode& node = orders_[handle];
    PriceLevel& level = *node.level;
    BookSide& book_side = node.side == Side::Buy ? bids_ : asks_;
    const bool inside = is_in_band(node.price_tick);
    const uint32_t index = inside ? band_index(node.price_tick) : 0;
    const int64_t price_tick = node.price_tick;

    if (node.prev == kInvalid) {
      level.head = node.next;
    } else {
      orders_[node.prev].next = node.next;
    }
    if (node.next == kInvalid) {
      level.tail = node.prev;
    } else {
      orders_[node.next].prev = node.prev;
    }
    --level.count;
    id_index_.erase(node.id);
    --active_order_count_;
    release_slot(handle);

    if (level.count == 0) {
      if (inside) {
        deactivate_price(book_side, node.side, index);
      } else {
        book_side.outside.erase(price_tick);
      }
    }
  }

  static Event fill_event(uint64_t order_id,
                          uint64_t remaining_quantity) noexcept {
    return remaining_quantity == 0
               ? Event{EventKind::FullyFilled, order_id, 0, 0}
               : Event{EventKind::PartiallyFilled, order_id, remaining_quantity,
                       0};
  }

  static size_t validated_capacity(size_t capacity) {
    if (capacity == 0 || capacity >= static_cast<size_t>(kInvalid)) {
      throw std::invalid_argument(
          "Maximum live orders must fit in 32-bit handles");
    }
    return capacity;
  }

  static PriceBand make_band(int64_t reference_price_units) {
    if (reference_price_units <= 0) {
      throw std::invalid_argument("Reference price must be positive");
    }

    auto checked_multiply = [](uint64_t lhs, uint64_t rhs) -> uint64_t {
      if (rhs != 0 && lhs > std::numeric_limits<uint64_t>::max() / rhs) {
        throw std::invalid_argument(
            "LULD calculation overflows 64-bit integer");
      }
      return lhs * rhs;
    };

    constexpr uint64_t kBasisPoints = 10'000;
    const uint64_t reference = static_cast<uint64_t>(reference_price_units);
    const uint64_t denominator =
        checked_multiply(static_cast<uint64_t>(kTickSizeUnits), kBasisPoints);
    const uint64_t lower_numerator =
        checked_multiply(reference, kBasisPoints - kLuldBps);
    const uint64_t upper_numerator =
        checked_multiply(reference, kBasisPoints + kLuldBps);

    uint64_t lower_tick = lower_numerator / denominator;
    if (lower_numerator % denominator != 0) {
      ++lower_tick; // Ceil the lower LULD boundary to a legal tick.
    }
    lower_tick = std::max(uint64_t{1}, lower_tick);
    const uint64_t upper_tick = upper_numerator / denominator;

    if (upper_tick < lower_tick ||
        upper_tick > static_cast<uint64_t>(std::numeric_limits<int64_t>::max() /
                                           kTickSizeUnits)) {
      throw std::invalid_argument(
          "LULD band has no representable positive ticks");
    }

    const uint64_t level_count = upper_tick - lower_tick + 1;
    if (level_count > kMaxInBandLevels) {
      throw std::invalid_argument("LULD band exceeds 262144 price ticks");
    }
    return PriceBand{static_cast<int64_t>(lower_tick),
                     static_cast<int64_t>(upper_tick),
                     static_cast<size_t>(level_count)};
  }

  const PriceBand band_; // fixed LULD price boundaries and tick-index mapping
  BookSide bids_;        // buy-side order book
  BookSide asks_;        // sell-side order book
  std::vector<OrderNode> orders_; // preallocated storage for resting orders
  OrderIdIndex id_index_; // maps active order IDs to their handles in orders_
  EventSink& event_sink_; // receives trade and order-fill events
  uint32_t next_unused_ = 0; // index of the next never-used slot in orders_
  uint32_t free_head_ = kInvalid; // head of the free list of order slots
  size_t active_order_count_ = 0; // number of currently active resting orders
};

} // namespace matching
