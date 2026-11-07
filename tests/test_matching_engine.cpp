#include "matching_engine.hpp"

#include <algorithm>
#include <cassert>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class CapturedEvents final {
public:
  explicit CapturedEvents(uint64_t price_scale = 1,
                          uint64_t tick_size_units = 1)
      : sink_(output_, price_scale, tick_size_units) {}

  matching::EventSink& sink() noexcept { return sink_; }

  std::string text() {
    assert(sink_.flush() == matching::Status::Ok);
    return output_.str();
  }

private:
  std::ostringstream output_;
  matching::EventSink sink_;
};

void test_assignment_example() {
  CapturedEvents capture;
  matching::MatchingEngine<1, 1000> engine(1010, 100, capture.sink());
  using matching::Side;
  using matching::Status;

  assert(engine.add_order(1000000, Side::Sell, 1, 1075) == Status::Ok);
  assert(engine.add_order(1000001, Side::Buy, 9, 1000) == Status::Ok);
  assert(engine.add_order(1000002, Side::Buy, 30, 975) == Status::Ok);
  assert(engine.add_order(1000003, Side::Sell, 10, 1050) == Status::Ok);
  assert(engine.add_order(1000004, Side::Buy, 10, 950) == Status::Ok);
  assert(engine.add_order(1000005, Side::Sell, 2, 1025) == Status::Ok);
  assert(engine.add_order(1000006, Side::Buy, 1, 1000) == Status::Ok);
  assert(engine.cancel_order(1000004) == Status::Ok);
  assert(engine.add_order(1000007, Side::Sell, 5, 1025) == Status::Ok);
  assert(engine.add_order(1000008, Side::Buy, 3, 1050) == Status::Ok);

  assert(capture.text() == "2,2,1025\n"
                           "4,1000008,1\n"
                           "3,1000005\n"
                           "2,1,1025\n"
                           "3,1000008\n"
                           "4,1000007,4\n");
  assert(engine.resting_quantity(1000007) == 4);
  assert(!engine.resting_quantity(1000005));
  assert(engine.best_price_units(Side::Sell) == 1025);
  assert(engine.best_price_units(Side::Buy) == 1000);
}

void test_outside_and_bands() {
  CapturedEvents capture;
  matching::MatchingEngine<1, 500> engine(1000, 8, capture.sink());
  using matching::Side;
  using matching::Status;
  assert(engine.lower_price_tick() == 950 && engine.upper_price_tick() == 1050);
  assert(engine.add_order(1, Side::Buy, 50, 1100) == Status::Ok);
  assert(engine.add_order(2, Side::Sell, 50, 1000) == Status::Ok);
  assert(engine.best_price_units(Side::Buy) == std::nullopt);
  assert(engine.best_price_units(Side::Sell) == 1000);
  assert(engine.add_order(3, Side::Sell, 10, 900) == Status::Ok);
  assert(engine.add_order(4, Side::Buy, 50, 1000) == Status::Ok);
  assert(capture.text() == "2,50,1000\n3,4\n3,2\n");
  assert(engine.active_order_count() == 2);
  assert(engine.resting_quantity(1) == 50);
  assert(engine.resting_quantity(3) == 10);
  assert(engine.cancel_order(1) == Status::Ok);
  assert(engine.cancel_order(3) == Status::Ok);
  assert(engine.active_order_count() == 0);
  assert(engine.cancel_order(1) == Status::UnknownOrderId);
}

void test_bitmap_boundaries_and_capacity() {
  CapturedEvents capture;
  matching::MatchingEngine<1, 5000> engine(200000, 16, capture.sink());
  using matching::Side;
  using matching::Status;
  assert(engine.lower_price_tick() == 100000 &&
         engine.upper_price_tick() == 300000);
  assert(engine.add_order(1, Side::Sell, 1, 100000) == Status::Ok);
  assert(engine.add_order(2, Side::Sell, 1, 104096) == Status::Ok);
  assert(engine.add_order(3, Side::Sell, 1, 165536) == Status::Ok);
  assert(engine.add_order(4, Side::Sell, 1, 299999) == Status::Ok);
  assert(engine.best_price_units(Side::Sell) == 100000);
  assert(engine.cancel_order(1) == Status::Ok);
  assert(engine.best_price_units(Side::Sell) == 104096);
  assert(engine.cancel_order(2) == Status::Ok);
  assert(engine.best_price_units(Side::Sell) == 165536);
  assert(engine.cancel_order(3) == Status::Ok);
  assert(engine.best_price_units(Side::Sell) == 299999);
  assert(engine.cancel_order(4) == Status::Ok);
  assert(!engine.best_price_units(Side::Sell));
  bool thrown = false;
  try {
    matching::MatchingEngine<1, 5000> too_wide(300000, 16, capture.sink());
    (void)too_wide;
  } catch (const std::invalid_argument&) {
    thrown = true;
  }
  assert(thrown);

  matching::MatchingEngine<1, 500> tight(1000, 1, capture.sink());
  assert(tight.add_order(10, Side::Buy, 10, 975) == Status::Ok);
  assert(tight.add_order(11, Side::Buy, 10, 974) ==
         Status::OrderCapacityExceeded);
  assert(tight.add_order(12, Side::Sell, 10, 975) == Status::Ok);
  assert(tight.active_order_count() == 0);
}

void test_multi_level_sweep() {
  CapturedEvents capture;
  matching::MatchingEngine<1, 500> engine(1000, 32, capture.sink());
  using matching::Side;
  using matching::Status;
  assert(engine.add_order(1, Side::Sell, 2, 960) == Status::Ok);
  assert(engine.add_order(2, Side::Sell, 1, 970) == Status::Ok);
  assert(engine.add_order(3, Side::Sell, 3, 990) == Status::Ok);
  assert(engine.add_order(4, Side::Sell, 4, 1040) == Status::Ok);
  assert(engine.add_order(5, Side::Buy, 7, 1000) == Status::Ok);
  assert(capture.text() == "2,2,960\n4,5,5\n3,1\n"
                           "2,1,970\n4,5,4\n3,2\n"
                           "2,3,990\n4,5,1\n3,3\n");
  assert(engine.best_price_units(Side::Sell) == 1040);
  assert(engine.best_price_units(Side::Buy) == 1000);
  assert(engine.resting_quantity(5) == 1);
  assert(engine.cancel_order(5) == Status::Ok);
  assert(!engine.best_price_units(Side::Buy));
}

void test_tick_scale_and_validation() {
  CapturedEvents capture(1'000'000, 10'000);
  matching::MatchingEngine<10'000, 500> engine(100'000'000, 16, capture.sink());
  using matching::Side;
  using matching::Status;
  assert(engine.lower_price_tick() == 9500);
  assert(engine.upper_price_tick() == 10500);
  assert(engine.add_order(1, Side::Buy, 1, 95'000'001) == Status::InvalidPrice);
  assert(engine.add_order(1, Side::Buy, 1, 95'000'000) == Status::Ok);
  assert(engine.best_price_units(Side::Buy) == 95'000'000);
  assert(engine.add_order(1, Side::Buy, 1, 95'000'000) ==
         Status::DuplicateActiveOrderId);
  assert(engine.add_order(2, Side::Sell, 1, 95'000'000) == Status::Ok);
  assert(capture.text() == "2,1,95.00\n3,2\n3,1\n");
  assert(engine.cancel_order(1) == Status::UnknownOrderId);
  assert(engine.add_order(0, Side::Buy, 1, 95'000'000) ==
         Status::InvalidOrderId);
  assert(engine.add_order(3, Side::Buy, 0, 95'000'000) ==
         Status::InvalidQuantity);
  assert(engine.add_order(3, static_cast<Side>(11), 1, 95'000'000) ==
         Status::InvalidSide);
  assert(engine.cancel_order(0) == Status::InvalidOrderId);
}

void test_full_bitmap_range() {
  matching::OccupancyBitmap3 bitmap;
  assert(bitmap.empty());
  bitmap.set(0);
  bitmap.set(63);
  bitmap.set(64);
  bitmap.set(4096);
  bitmap.set(262143);
  assert(bitmap.lowest() == 0);
  assert(bitmap.highest() == 262143);
  bitmap.clear(0);
  bitmap.clear(63);
  bitmap.clear(64);
  assert(bitmap.lowest() == 4096);
  bitmap.clear(262143);
  assert(bitmap.highest() == 4096);
  bitmap.clear(4096);
  assert(bitmap.empty());
}

void test_hash_churn() {
  CapturedEvents capture;
  matching::MatchingEngine<1, 1000> engine(1000, 256, capture.sink());
  std::mt19937_64 generator(17);
  std::vector<uint64_t> live;
  uint64_t next_id = 1;
  for (size_t i = 0; i < 200000; ++i) {
    const bool add =
        live.empty() || (live.size() < 256 && generator() % 5 != 0);
    if (add) {
      const uint64_t id = next_id++;
      assert(engine.add_order(id, matching::Side::Buy, 1, 1000) ==
             matching::Status::Ok);
      live.push_back(id);
    } else {
      const size_t index = static_cast<size_t>(generator() % live.size());
      const uint64_t id = live[index];
      assert(engine.resting_quantity(id) == 1);
      assert(engine.cancel_order(id) == matching::Status::Ok);
      live[index] = live.back();
      live.pop_back();
    }
    assert(engine.active_order_count() == live.size());
  }
  for (uint64_t id : live) {
    assert(engine.cancel_order(id) == matching::Status::Ok);
  }
  assert(engine.active_order_count() == 0);
  assert(capture.text().empty());
}

void test_sink_decimal_and_stream() {
  CapturedEvents capture(1'000'000, 1);
  using matching::Event;
  using matching::EventKind;
  capture.sink().emit(Event{EventKind::Trade, 0, 2, 100'010'000});
  capture.sink().emit(Event{EventKind::Trade, 0, 3, 95'000'001});
  capture.sink().emit(Event{EventKind::PartiallyFilled, 123, 42, 0});
  capture.sink().emit(Event{EventKind::FullyFilled, 123, 0, 0});
  for (size_t i = 0; i < 10'000; ++i) {
    capture.sink().emit(Event{EventKind::Trade, 0, 1, 1'250'000});
  }
  const std::string output = capture.text();
  const std::string prefix = "2,2,100.010000\n2,3,95.000001\n4,123,42\n3,123\n";
  assert(output.starts_with(prefix));
  assert(std::count(output.begin(), output.end(), '\n') == 10'004);
  assert(output.ends_with("2,1,1.250000\n"));

  CapturedEvents cents(1'000'000, 10'000);
  cents.sink().emit(Event{EventKind::Trade, 0, 5, 100'010'000});
  cents.sink().emit(Event{EventKind::Trade, 0, 1, 95'000'000});
  assert(cents.text() == "2,5,100.01\n2,1,95.00\n");
}

} // namespace

int main() {
  test_assignment_example();
  test_outside_and_bands();
  test_bitmap_boundaries_and_capacity();
  test_multi_level_sweep();
  test_tick_scale_and_validation();
  test_full_bitmap_range();
  test_hash_churn();
  test_sink_decimal_and_stream();
  std::cout << "All tests passed" << std::endl;
  return 0;
}
