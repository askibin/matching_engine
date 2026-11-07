#pragma once

#include <cstdint>

namespace matching {

enum class Side : uint8_t { Buy, Sell };

enum class EventKind : uint8_t {
  Trade = 2,
  FullyFilled = 3,
  PartiallyFilled = 4
};

struct Event {
  EventKind kind{};
  uint64_t order_id{}; // Zero for trade events.
  uint64_t
      quantity{}; // Trade quantity or remaining quantity for partial fills.
  int64_t price_units{}; // Trade price; ignored for fill events.
};

enum class Status : uint8_t {
  Ok,
  InvalidOrderId,
  InvalidSide,
  InvalidQuantity,
  InvalidPrice,
  DuplicateActiveOrderId,
  UnknownOrderId,
  OrderCapacityExceeded,
  AllocationFailed,
  OutputError
};

} // namespace matching
