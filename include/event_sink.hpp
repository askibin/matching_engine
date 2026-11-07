#pragma once

#include "matching_types.hpp"

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <ostream>
#include <stdexcept>

namespace matching {

// Emits the assignment's CSV events using the standard output stream.
// Both the internal price scale and tick size use the same integer units.
// The tick size determines the number of digits after the decimal point.
class EventSink final {
public:
  explicit EventSink(std::ostream& output = std::cout, uint64_t price_scale = 1,
                     uint64_t tick_size_units = 1)
      : output_(output), price_scale_(price_scale) {
    if (tick_size_units == 0 || price_scale_ == 0) {
      throw std::invalid_argument("Tick size and price scale must be positive");
    }

    fraction_divisor_ = price_scale_;
    while (tick_size_units % fraction_divisor_ != 0) {
      fraction_divisor_ /= 10;
      ++decimal_digits_;
    }
  }

  void emit(const Event& event) {
    output_ << static_cast<unsigned>(event.kind) << ',';
    switch (event.kind) {
    case EventKind::Trade:
      output_ << event.quantity << ',';
      write_price(event.price_units);
      break;
    case EventKind::FullyFilled:
      output_ << event.order_id;
      break;
    case EventKind::PartiallyFilled:
      output_ << event.order_id << ',' << event.quantity;
      break;
    }
    output_ << '\n';
  }

  Status status() const noexcept {
    return output_ ? Status::Ok : Status::OutputError;
  }

  Status flush() {
    output_.flush();
    return status();
  }

private:
  void write_price(int64_t price_units) {
    output_ << price_units / price_scale_;

    if (decimal_digits_ > 0) {
      const auto fraction = (price_units % price_scale_) / fraction_divisor_;
      output_ << '.' << std::setfill('0') << std::setw(decimal_digits_)
              << fraction;
    }
  }

  std::ostream& output_;
  uint64_t price_scale_;
  uint64_t fraction_divisor_ = 1;
  size_t decimal_digits_ = 0;
};

} // namespace matching
