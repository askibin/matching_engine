# MATCHING ENGINE

Single-threaded, in-memory limit order book optimized for low-latency
price-time priority matching.

Limit Up-Limit Down (LULD) is a U.S. market-wide safeguard used by exchanges
such as NYSE and Nasdaq to prevent individual stocks and ETFs from trading
outside specified price bands around a reference price. The matching engine
leverages these boundaries to optimize order book performance by dividing
it into two regions: an in-band book for executable orders and an out-of-band
book for orders that can only be added or canceled. The in-band book uses
direct-indexed price levels and a 3-level hierarchical occupancy bitmap,
supporting up to 262,144 price ticks. This bounded range enables O(1)
price-level access and best-price discovery without tree traversal.
Out-of-band orders are stored separately in std::map containers, avoiding
the memory overhead of allocating price levels across the entire price
domain. This hybrid design concentrates memory and CPU optimizations on
the executable price range while preserving support for distant prices.

Price Management:
- Integer tick-based prices, no floating-point arithmetic in the hot path.
- LULD percentage and minimum tick size are compile-time template parameters.
- The LULD price range is calculated from a runtime reference price.
- Supports up to 262,144 (64^3) discrete price levels inside LULD.
- In-band prices use direct-indexed arrays and 3-level hierarchical
occupancy bitmaps (64-bit words) for each side of the book.
- Cached best bid/ask indices provide O(1) crossing checks.
- Bitmaps provide O(1) best-price discovery when levels become empty.
- Prices outside LULD are stored in separate std::map containers per side.
Out-of-band orders support insertion and cancellation, but not matching.

Order Management:
- Resting orders use intrusive doubly linked FIFO queues per price level,
preserving price-time priority.
- Orders reside in a preallocated arena with stable handles.
- A global flat hash table maps order IDs to handles for O(1) expected lookup.
- Aggressive orders are matched directly without allocation, only unfilled
quantities are inserted into the book.
- Fully filled orders are unlinked and their arena slots recycled.

Performance:
- In-band price lookup, insertion, removal, and best-price discovery: O(1).
- Matching K resting orders: O(K).
- Out-of-band insertion: O(log M), where M is the number of occupied
out-of-band price levels.
- Out-of-band cancellation: O(1), or O(log M) when removing
an empty price level.

Files:
- `main.cpp`: CLI tool.
- `matching_engine.hpp`: matching engine implementation.
- `occupancy_bitmap3.hpp`: three-level hierarchical occupancy index for in-band prices.
- `event_sink.hpp`: events writer.
- `matching_types.hpp`: public types.
- `test_matching_engine.cpp`: unit tests.
- `sample_input.csv`, `expected_sample_output.csv`: assignment's examples.

Build:
```
g++ -std=c++23 -O3 -Wall -Wextra -Wpedantic -I include src/main.cpp -o bin/matching_engine
g++ -std=c++23 -O3 -Wall -Wextra -Wpedantic -I include tests/test_matching_engine.cpp -o bin/test_matching_engine
```

Run:
```
bin/matching_engine --reference-price 1010 --tick-size 1 --luld-bps 1000 --max-orders 100000 < examples/sample_input.csv
bin/test_matching_engine
```