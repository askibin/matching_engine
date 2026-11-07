#include "matching_engine.hpp"

#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

// Prices inside the engine are integers, it's up to the caller how to scale
// them.
constexpr int64_t kPriceScale = 1'000'000;

struct Options {
  int64_t reference_price_units = 0;
  int64_t tick_size_units = 0;
  uint32_t luld_bps = 0;
  size_t max_orders = 100000;
};

// Convert decimal values into the engine's units.
int64_t to_price_units(double price) {
  const double scaled = price * static_cast<long double>(kPriceScale);
  if (!std::isfinite(scaled) || scaled <= 0 ||
      scaled >= static_cast<long double>(std::numeric_limits<int64_t>::max())) {
    throw std::invalid_argument("Price is out of range");
  }
  return static_cast<int64_t>(std::round(scaled));
}

int64_t parse_price_argument(const std::string& text) {
  size_t consumed = 0;
  const double price = std::stold(text, &consumed);
  if (consumed != text.size()) {
    throw std::invalid_argument("Invalid price: " + text);
  }
  return to_price_units(price);
}

int64_t parse_integer_argument(const std::string& text) {
  size_t consumed = 0;
  const int64_t value = std::stoll(text, &consumed);
  if (consumed != text.size()) {
    throw std::invalid_argument("Invalid integer: " + text);
  }
  return value;
}

void print_usage() {
  std::cerr
      << "Usage: matching_engine --reference-price PRICE --tick-size TICK "
         "--luld-bps BPS [--max-orders COUNT] < requests.csv\n"
      << "Compiled tick sizes: 0.0001, 0.001, 0.005, 0.01, 0.05, 1\n"
      << "Compiled LULD bands: 500, 1000, 2000 basis points\n"
      << "Input: 0,id,side,quantity,price (side: 0=buy, 1=sell) or 1,id\n";
}

std::optional<Options> parse_options(int argc, char* argv[]) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string param = argv[i];
    if (param == "--help" || param == "-h") {
      print_usage();
      return std::nullopt;
    }
    if (++i == argc) {
      throw std::invalid_argument("Missing value for " + param);
    }
    const std::string value = argv[i];
    if (param == "--reference-price") {
      options.reference_price_units = parse_price_argument(value);
    } else if (param == "--tick-size") {
      options.tick_size_units = parse_price_argument(value);
    } else if (param == "--luld-bps") {
      const int64_t bps = parse_integer_argument(value);
      if (bps < 0 || bps >= 10'000) {
        throw std::invalid_argument("Invalid LULD basis points");
      }
      options.luld_bps = static_cast<uint32_t>(bps);
    } else if (param == "--max-orders") {
      const int64_t count = parse_integer_argument(value);
      if (count <= 0 || count >= std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument("Invalid maximum order count");
      }
      options.max_orders = static_cast<size_t>(count);
    } else {
      throw std::invalid_argument("Unknown argument: " + param);
    }
  }
  if (options.reference_price_units == 0 || options.tick_size_units == 0 ||
      options.luld_bps == 0) {
    throw std::invalid_argument("Missing required option (see --help)");
  }
  return options;
}

const char* status_message(matching::Status status) {
  switch (status) {
  case matching::Status::Ok:
    return "Ok";
  case matching::Status::InvalidOrderId:
    return "Invalid order ID";
  case matching::Status::InvalidSide:
    return "Invalid side";
  case matching::Status::InvalidQuantity:
    return "Invalid quantity";
  case matching::Status::InvalidPrice:
    return "Invalid price or unsupported tick";
  case matching::Status::DuplicateActiveOrderId:
    return "Duplicate active order ID";
  case matching::Status::UnknownOrderId:
    return "Unknown or already removed order ID";
  case matching::Status::OrderCapacityExceeded:
    return "Order capacity exceeded";
  case matching::Status::AllocationFailed:
    return "Memory allocation failed";
  case matching::Status::OutputError:
    return "Output stream failed";
  }
  return "Unknown status";
}

// A stream-based CSV parser.
template <class Book>
matching::Status process_request(const std::string& line, Book& book) {
  std::istringstream input(line);
  std::string type_text;
  std::getline(input, type_text, ',');

  int message_type = -1;
  std::istringstream type_input(type_text);
  if (!(type_input >> message_type) || !(type_input >> std::ws).eof() ||
      (message_type != 0 && message_type != 1)) {
    throw std::invalid_argument("Unknown message type: " + type_text);
  }
  if (input.eof()) {
    throw std::invalid_argument("Expected message_type,order_id");
  }

  uint64_t order_id = 0;
  input >> std::ws;
  if (input.peek() == '-' || !(input >> order_id)) {
    throw std::invalid_argument("Invalid order ID");
  }

  if (message_type == 1) {
    input >> std::ws;
    if (!input.eof()) {
      throw std::invalid_argument("Cancel format: 1,order_id");
    }
    return book.cancel_order(order_id);
  }

  int side = -1;
  uint64_t quantity = 0;
  double price = 0;
  char side_separator = 0;
  char quantity_separator = 0;
  char price_separator = 0;
  if (!(input >> side_separator >> side >> quantity_separator) ||
      side_separator != ',' || quantity_separator != ',') {
    throw std::invalid_argument("Add format: 0,id,side,quantity,price");
  }
  input >> std::ws;
  if (input.peek() == '-' || !(input >> quantity >> price_separator >> price) ||
      price_separator != ',') {
    throw std::invalid_argument("Invalid quantity or price");
  }
  if (side != 0 && side != 1) {
    throw std::invalid_argument("Invalid side");
  }
  input >> std::ws;
  if (!input.eof()) {
    throw std::invalid_argument("Unexpected characters after price");
  }
  return book.add_order(order_id, static_cast<matching::Side>(side), quantity,
                        to_price_units(price));
}

template <int64_t kTickSizeUnits, uint32_t kLuldBps>
int run_book(const Options& options) {
  matching::EventSink event_sink(std::cout, kPriceScale, kTickSizeUnits);
  matching::MatchingEngine<kTickSizeUnits, kLuldBps> book(
      options.reference_price_units, options.max_orders, event_sink);

  std::string line;
  size_t line_number = 0;
  while (std::getline(std::cin, line)) {
    ++line_number;
    if (line.find_first_not_of(" \t\r") == std::string::npos) {
      std::cerr << "Line " << line_number << ": Empty input message\n";
      continue;
    }
    try {
      const matching::Status status = process_request(line, book);
      if (status == matching::Status::OutputError) {
        std::cerr << "Line " << line_number << ": output stream failed\n";
        return 1;
      }
      if (status != matching::Status::Ok) {
        std::cerr << "Line " << line_number << ": " << status_message(status)
                  << '\n';
      }
    } catch (const std::exception& error) {
      std::cerr << "Line " << line_number << ": " << error.what() << '\n';
    }
  }
  if (!std::cin.eof()) {
    std::cerr << "Failed reading stdin\n";
    return 1;
  }
  if (event_sink.flush() != matching::Status::Ok) {
    std::cerr << "Failed writing stdout\n";
    return 1;
  }
  return 0;
}

template <int64_t kTickSizeUnits> int dispatch_luld(const Options& options) {
  switch (options.luld_bps) {
  case 500:
    return run_book<kTickSizeUnits, 500>(options);
  case 1000:
    return run_book<kTickSizeUnits, 1000>(options);
  case 2000:
    return run_book<kTickSizeUnits, 2000>(options);
  default:
    throw std::invalid_argument("Unsupported LULD band");
  }
}

int dispatch_tick_size(const Options& options) {
  switch (options.tick_size_units) {
  case 100:
    return dispatch_luld<100>(options); // $0.0001
  case 1'000:
    return dispatch_luld<1'000>(options); // $0.001
  case 5'000:
    return dispatch_luld<5'000>(options); // $0.005
  case 10'000:
    return dispatch_luld<10'000>(options); // $0.01
  case 50'000:
    return dispatch_luld<50'000>(options); // $0.05
  case 1'000'000:
    return dispatch_luld<1'000'000>(options); // $1
  default:
    throw std::invalid_argument("Unsupported tick size");
  }
}

} // namespace

int main(int argc, char* argv[]) {
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);

  try {
    const std::optional<Options> options = parse_options(argc, argv);
    return options ? dispatch_tick_size(*options) : 0;
  } catch (const std::exception& error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 2;
  }
}
