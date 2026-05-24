# hffix2

Fork of [jamesdbrock/hffix](https://github.com/jamesdbrock/hffix).
Header-only, in-place, no-alloc FIX parser. Aims at lower latency and
tighter tails than upstream: SoA field index, SWAR digit parsing,
O(1) data-tag lookup, counter-based asserts under `NDEBUG`, no
exceptions anywhere in the library, zero allocations.

Adds a Google Benchmark suite under `benchmarks/` (upstream ships
none); see [Benchmarks](#benchmarks).

Changes from upstream:

- `is_tag_a_data_length` is an O(1) bitmap lookup (upstream walks ~75
  ints per field).
- SWAR timestamp parser instead of digit-by-digit.
- Reader iterator carries `message_end` so `increment()` doesn't chase
  the parent reader.
- `basic_indexed_message` + `build_field_index` with caller-supplied
  `field_index_buffer<N>`. Insertion-order SoA index (`tags[N]` +
  `pos_len[N]`).
  Two error flags: `truncated()` (more fields than `N`) and
  `overflowed()` (message > 4 GiB; field position/length each need to
  fit in `uint32_t`). `authoritative()` combines them.
  See [Field indexing](#field-indexing) for usage and sizing.
- Compile-time group dispatch: `reader.group<tag::NoMDEntries>()` looks
  the delimiter up in `hffix::groups::group_def<CountTag>`. All FIX 5.0
  SP2 + FIXT 1.1 groups (~500) generated from the spec by
  `fixspec-gen-fields` into `hffix_groups.hpp`. Runtime
  `group(count_tag, first_tag)` for ad-hoc cases and for the handful
  of NoXxx names whose delimiter differs across messages. Iterator is
  bounded by the NoXxx count.
- `message_writer` is `noexcept`. `push_back_*` set an internal error
  flag on overflow and become no-ops; caller calls `push_back_trailer()` once
  and checks the `bool` return. The throwing variant is gone.
- Batched parse over a single buffer: `hffix::messages(buf, n)` range
  and `hffix::for_each_message(begin, end, fn)` callback. Yield only
  complete + valid readers, skip invalid frames via
  `next_message_reader` resync, and surface the tail pointer
  (`iterator::remainder()` / return value) for stream re-feeding.
  See [Batched parse](#batched-parse).
- Reader programmer-error checks (`begin()` on an invalid message,
  `next_message_reader()` on incomplete, etc.) expand to
  `HFFIX_ASSERT(cond, msg)`. Under `NDEBUG` it bumps an atomic counter
  (`hffix::assert_failure_count()`); in debug builds it traps. Override
  the macro before include for a custom handler.
- Boost.DateTime dropped. Writer takes `std::chrono::sys_time` or a
  raw `int64_t` epoch (`push_back_timestamp_epoch_nanos` / `_millis`);
  reader returns `std::optional<int64_t>` from `as_epoch_nanos` /
  `as_epoch_millis`. Epoch-materializing
  reader paths reject `year` outside `[1970, 2200]` to avoid silent
  `int64_t` overflow in `chrono`'s nanosecond arithmetic; see
  [Timestamp range](#timestamp-range).
- `field_value::as_string()` removed; use `as_string_view()` or
  `begin()`/`end()`. Library is strictly no-alloc end-to-end.
- Non-validating `field_value::as_int<T>()` / `as_decimal<T>()` renamed to
  `as_int_unchecked<T>()` / `as_decimal_unchecked<T>()`. Non-digit input
  produces undefined output; no overflow detection. Use `try_as_int(T&)`
  / `try_as_decimal` for any wire-untrusted value.
- CMake + Conan 2. GoogleTest + Google Benchmark.
- C++20 required; legacy `__cplusplus` guards removed.
- `fixgen` writes synthetic FIX 5.0 SP2 datasets over FIXT 1.1.
  MD snapshots: 3–10 book levels per side. MD incrementals: 1–200
  entries. Timestamps carry millisecond jitter so the SWAR parser
  sees varied digits. NewOrderSingle / ExecutionReport / Logon /
  Logout / Heartbeat are sampled alongside MD; `ClOrdID` is a
  20-char alphanumeric.
- `fixspec-gen-fields` rewritten from Haskell to C++ (pugixml) and
  switched to QuickFIX-format spec XML as input (`FIX50SP2.xml` +
  `FIXT11.xml` ship in `fixspec/`). Drops the Haskell/Cabal toolchain;
  ships as a regular CMake target alongside the rest of the utils.
- `.clang-format`, `scripts/format{,-check}.sh`.

## Build

CMake 3.20+, Conan 2.x, C++20 toolchain.

```sh
pip install 'conan>=2.0,<3.0'
conan profile detect --force

conan install . --output-folder=build --build=missing \
    -s build_type=Release \
    -s compiler.cppstd=gnu20 -s:b compiler.cppstd=gnu20
cmake --preset conan-release
cmake --build build -j
ctest --test-dir build
```

CLI binaries land in `build/utils/`:

```sh
build/utils/fixgen -o data.fix -n 500000
build/utils/fixprint < data.fix | head
```

## Consuming as a Conan 2 package

`conanfile.py` declares `hffix2/1.0.0` as a header-only package with
CMake target `hffix2::hffix` and CMake file name `hffix2`.

```sh
conan create . --build=missing \
    -s compiler.cppstd=gnu20 -s:b compiler.cppstd=gnu20 \
    -c tools.build:skip_test=True
```

Downstream `conanfile.txt`:

```
[requires]
hffix2/1.0.0
[generators]
CMakeDeps
CMakeToolchain
```

Downstream `CMakeLists.txt`:

```cmake
find_package(hffix2 REQUIRED)
target_link_libraries(my_app PRIVATE hffix2::hffix)
```

## Benchmarks

Dataset and bench run are separate targets. The dataset is NOT committed
(would be multi-GB); generate it locally before running benchmarks:

```sh
cmake --build build --target bench_data    # writes benchmarks/data/synthetic.fix
cmake --build build --target bench         # runs benchmarks against it
```

`bench` does NOT depend on `bench_data`. Re-invoke the latter only when
seed, size, or generator change. Size is controlled via
`-DHFFIX_BENCH_MESSAGES=N` at configure time (default 500 000).

The bench suite registers:

- `BM_Parse_ScanMessages`: `next_message_reader()` only, no field work.
- `BM_Parse_IterAllFields`: visit every field of every message.
- `BM_Parse_FindCommonFields`: `find_with_hint` for 5 header tags.
- `BM_Parse_FindManyFields_Iter`: `find_with_hint` for 15 tags mixed
  across the message (10 present in MD bodies, 5 always absent →
  force-scans every field).
- `BM_Parse_FindLotsFields_Iter`: same idea, 30 tags (10 present, 20
  absent); iterator-style lookup worst case.
- `BM_Parse_FindManyFields_*_HighHit`: 15-tag set with ~55-65% per-tag
  hit rate against fixgen's mix (iter + indexed variants). Honest
  baseline against the absent-bias above.
- `BM_Parse_FindCommonFields_Indexed` / `BM_Parse_FindManyFields_Indexed`
  / `BM_Parse_FindLotsFields_Indexed`: same lookup workloads via
  `build_field_index` + `idx.find_with_hint`.
- `BM_Parse_BuildFieldIndex`: index build only, no lookups.
- `BM_Parse_FindN_Iter/<n>` / `BM_Parse_FindN_Indexed/<n>`: per-lookup
  amortization for `n ∈ {3,5,7,8,10,13,15,20,30}`.
- `BM_Parse_Sequential_Iter` / `BM_Parse_Random_Iter` /
  `BM_Parse_Sequential_Indexed` / `BM_Parse_Random_Indexed`: 10 tags
  read in on-wire order vs reversed. Exercises `find_with_hint` carry
  across calls (sequential = forward, random = wrap-heavy).
- `BM_Parse_TailLatency_{Sequential,Random}_{Iter,Indexed}`: same
  pattern, per-message timing via `clock::now()` probes. Reports
  p95/p99/p999/max. Probe adds ~20-30 ns/sample uniformly; relative
  deltas comparable, absolute p* not.
- `BM_Parse_GroupAccess_Sugared` / `BM_Parse_GroupAccess_Manual`:
  same `MDEntryPx` + `MDEntrySize` per-entry lookups, via
  `reader.group<>()` and via hand-rolled delimiter scan. Throughput
  parity check on the sugar.
- Single-message hot path: `BM_WriteLogon`, `BM_WriteNewOrder`,
  `BM_WriteNewOrder_EpochNanos`, `BM_WriteNewOrder_Closure`,
  `BM_ReadMessageScan`, `BM_ReadMessageFindFields`, `BM_RoundTrip`,
  `BM_ParseTimestampMillis`, `BM_ParseTimestampNanos`.

### Numbers

Single-threaded, `-O3 -flto=full`, native arch flag enabled. Synthetic
dataset (`utils/fixgen`, default 500 000 messages, avg 1222 B/msg):
mixed MD snapshots, MD incrementals up to 200 entries, NewOrderSingle,
ExecutionReport, session messages. Mean of 5 repetitions at
`--benchmark_min_time=1s`.

#### ARM64 / Apple M4

Iterator path (upstream API surface, like-for-like):

| Benchmark | upstream | fork |
| --- | --- | --- |
| `ScanMessages` | 21.1 M msgs/s | **21.4 M (+2%)** |
| `IterAllFields` | 122 M f/s | **218 M (+78%)** |
| `FindCommonFields(5)` | 16.3 M msgs/s | **18.1 M (+11%)** |
| `FindManyFields(15)` | 0.143 M msgs/s | **0.251 M (+76%)** |
| `FindLotsFields(30)` | 0.0415 M msgs/s | **0.0762 M (+84%)** |

Indexed (`field_index_buffer<N>` path):

| Benchmark | fork idx |
| --- | --- |
| `BuildFieldIndex` (no lookups) | 1.42 M msgs/s |
| `FindCommonFields(5) idx` | 1.43 M msgs/s (-92% vs iter) |
| `FindManyFields(15) idx` | 1.05 M msgs/s (+319% vs iter) |
| `FindLotsFields(30) idx` | 0.668 M msgs/s (+777% vs iter) |

Realistic hit-rate (15 tags, ~55-65% per-tag hit on fixgen's mix; no
absent-bias inflation):

| Benchmark | fork |
| --- | --- |
| `FindManyFields_Iter_HighHit` | 0.166 M msgs/s |
| `FindManyFields_Indexed_HighHit` | 0.949 M msgs/s (+472% vs iter) |

Group access (10-entry MD increment payload, `MDEntryPx` + `MDEntrySize`
per entry):

| Benchmark | fork |
| --- | --- |
| `GroupAccess_Sugared` (`reader.group<>()`) | 0.807 M msgs/s |
| `GroupAccess_Manual` (hand-rolled delimiter scan) | 0.826 M msgs/s |

Sugar within 2% of the manual baseline; difference inside run-to-run
noise.

Single-message hot path (in-cache):

| Benchmark | upstream | fork |
| --- | --- | --- |
| `BM_WriteLogon` | 52.0 ns | **24.1 ns (-54%)** |
| `BM_WriteNewOrder` | 74.0 ns | **46.8 ns (-37%)** |
| `BM_WriteNewOrder_EpochNanos` | — | 49.6 ns |
| `BM_WriteNewOrder_Closure` | — | 46.3 ns |
| `BM_ReadMessageScan` | 90.5 ns | **59.4 ns (-34%)** |
| `BM_ReadMessageFindFields` | 91.4 ns | **55.0 ns (-40%)** |
| `BM_RoundTrip` | 169 ns | **99.6 ns (-41%)** |
| `BM_ParseTimestampMillis` | — | 7.29 ns |
| `BM_ParseTimestampNanos` | — | 7.17 ns |

`_EpochNanos` parity with `_NewOrder` (49.6 vs 46.8 ns) shows
`push_back_timestamp_epoch_nanos` has no overhead vs the chrono
`time_point` overload. `_Closure` (`try_write_message` lambda) parity
shows the closure helper is free.

Per-lookup-count break-even on a single NewOrder message in cache:

| Lookups | iterator | indexed |
| --- | --- | --- |
| 3 | **17.3 ns** | 62.2 ns |
| 5 | **44.6 ns** | 64.7 ns |
| 8 | 120 ns | **72.2 ns** |
| 13 | 353 ns | **92.4 ns** |
| 15 | 449 ns | **99.7 ns** |

Index amortization on the dataset (throughput in `M msgs/s`;
ratio = `indexed / iterator`):

| Lookups | iterator | indexed | idx/iter |
| --- | --- | --- | --- |
| 3  | **21.3 M** | 1.42 M | 0.07× |
| 5  | **18.2 M** | 1.41 M | 0.08× |
| 7  | **1.42 M** | 1.41 M | 0.99× |
| 8  | 1.33 M | **1.40 M** | 1.05× |
| 10 | 1.19 M | **1.37 M** | 1.16× |
| 13 | 0.358 M | **1.19 M** | 3.33× |
| 15 | 0.248 M | **1.09 M** | 4.40× |
| 20 | 0.139 M | **0.900 M** | 6.48× |
| 30 | 0.0730 M | **0.671 M** | 9.19× |

Break-even at ~8 lookups on the dataset; at 30 lookups indexed wins
~9× over iterator-style search.

Sequential vs random tag read (10 tags, in on-wire order vs reversed):

| Benchmark | upstream | fork |
| --- | --- | --- |
| `Sequential_Iter` | 0.755 M msgs/s | **1.22 M (+62%)** |
| `Random_Iter` | 0.138 M msgs/s | **0.236 M (+71%)** |
| `Sequential_Indexed` | — | 1.43 M msgs/s |
| `Random_Indexed` | — | 1.10 M msgs/s |

Random reads cost ~5× sequential under the iterator path; the indexed
path closes the gap to ~1.3× (random ~77% of sequential).

Tail latency per message. Includes ~20-30 ns `clock::now()` probe per
sample; same overhead in every variant, so relative deltas are
meaningful, absolute p* are not.

| Benchmark | p95 | p99 | p999 | max |
| --- | --- | --- | --- | --- |
| `Sequential_Iter` (fork) | 3780 ns | 4530 ns | 4840 ns | 29800 ns |
| `Random_Iter` (fork) | 22500 ns | 27100 ns | 29100 ns | 85100 ns |
| `Sequential_Indexed` (fork) | 4020 ns | 4820 ns | 5150 ns | 34100 ns |
| `Random_Indexed` (fork) | 5140 ns | 6180 ns | 7220 ns | 81700 ns |
| `Sequential_Iter` (upstream) | 6690 ns | 8030 ns | 8490 ns | 45700 ns |
| `Random_Iter` (upstream) | 39200 ns | 47100 ns | 50100 ns | 202000 ns |

`Random_Iter` tails blow up (max ~85 µs fork, ~202 µs upstream)
because every lookup forces a full message rescan on a 1.2 KB MD frame.
`Random_Indexed` keeps the tail within ~2× of `Sequential_Indexed`.

## Field indexing

`build_field_index` walks a message once and fills a caller-supplied
`field_index_buffer<N>` with two parallel arrays: `int tags[i]` and a
packed `uint64_t pos_len[i] = (uint32_t value_pos << 32) | uint32_t value_len`.
`indexed_message::find_with_hint` scans the `tags` array and unpacks
`pos_len` only at the match site. Pays off above ~6 lookups per
message; see [Benchmarks](#benchmarks).

### Failure modes

Two flags. Check both before treating `find_with_hint()` "absent" as real.

- `overflowed()`: message > 4 GiB, or any single field's position /
  length exceeds `uint32_t`. The packed `pos_len` halves cannot
  address past that. No index is built, tag/pos_len arrays empty.
  Iterator API still works on the message.
- `truncated()`: message has more fields than `field_index_buffer<N>::capacity`.
  Index holds the first `N` in insertion order; fields past slot `N`
  are invisible to `find_with_hint()`. Bump `N`, fall back to iterator, or
  accept the false-negative explicitly.

`idx.authoritative()` is shorthand for `!truncated() && !overflowed()`.
Gate every `find_with_hint()` on it.

### Usage

```cpp
hffix::field_index_buffer<512> idx_buffer;

for (auto r = hffix::message_reader(buf, buf + n);
     r.is_complete() && r.is_valid();
     r = r.next_message_reader()) {

    auto idx = hffix::build_field_index(r, idx_buffer);

    if (!idx.authoritative()) [[unlikely]] {
        hffix::message_reader::const_iterator it = r.begin();
        if (!r.find_with_hint(hffix::tag::Price, it)) continue;
        auto price = it->value();
        continue;
    }

    std::size_t h = 0;
    auto price = idx.find_with_hint(hffix::tag::Price, h);
    if (price.begin() == price.end()) continue;
}
```

Skipping the `authoritative()` check treats `find_with_hint() == empty`
as "absent" even when the field exists past slot `N`. Silent false
negative.

### Sizing `N`

`field_index_buffer<N>` is stack-allocated, `12*N` bytes (`int tags[N]`
\+ `uint64_t pos_len[N]`) plus up to 32 bytes of `alignas(32)` padding.
Oversize freely; assert `!truncated()` in dev.

- Session messages (Logon, Heartbeat, ResendRequest): `N=16`.
- Single-instrument order flow (NewOrderSingle, ExecutionReport):
  15–30 fields, `N=64`.
- Market data snapshots / incrementals: depends on `NoMDEntries`.
  `N=512` covers most feeds; full-book refreshes can exceed it.
- Multi-leg orders with nested groups: `N=128`+.

## Writing messages

`message_writer` is `noexcept` end-to-end. Each `push_back_*` checks
the remaining buffer; on overflow it flips an internal error flag and
becomes a no-op for the rest of the message. Caller checks `push_back_trailer()`
once at the end (returns `bool`). No exception escapes.

```cpp
#include <hffix.hpp>

char buf[1024];
hffix::message_writer w(buf);
w.push_back_header("FIXT.1.1");
w.push_back_string(hffix::tag::MsgType, "D");
w.push_back_int(hffix::tag::OrderQty, 100);
w.push_back_decimal(hffix::tag::Price, 50001, -2);
if (!w.push_back_trailer()) {
    // buffer was too small somewhere; nothing was sent.
    return;
}
char* end = w.message_end();
```

Each `push_back_*`: one bounds check against the field's worst-case
byte count, then unchecked writes through `*_unchecked` digit helpers
and `memcpy`. Poison check and bounds check are both
predicted-not-taken.

Closure helper:

```cpp
char* end;
bool ok = hffix::try_write_message(buf, buf + sizeof(buf), end,
    [&](hffix::message_writer& w) {
        w.push_back_header("FIXT.1.1");
        w.push_back_string(hffix::tag::MsgType, "D");
        w.push_back_int(hffix::tag::OrderQty, 100);
    });
```

## Error handling and `HFFIX_ASSERT`

Two error channels.

**1. Recoverable parse errors:** surfaced as state, never as
exceptions. On the reader: `is_complete()` / `is_valid()`. On the
writer: a sticky `has_error()` flag plus a `bool` return from
`push_back_trailer()`. Validating parsers return `bool` via the `try_as_*`
family. **None of these trap.** Counter++ in a metric, drop the frame,
log, and continue.

**2. Programmer-error preconditions:** expand to `HFFIX_ASSERT(cond,
msg)`. Default behavior depends on `NDEBUG`:

| Build | `NDEBUG` | `HFFIX_ASSERT` failure |
| --- | --- | --- |
| Release | defined | Atomic counter bump (lock-free, no abort, no allocation). |
| Debug | not defined | Trap: `__builtin_trap()` on gcc/clang, `__debugbreak()` on MSVC, `std::abort()` elsewhere. |

Read the counter:

```cpp
auto fails = hffix::assert_failure_count();
hffix::reset_assert_failure_count();
```

Benchmarks and the fuzz harness ship their own `assert_override.hpp`
that always `std::abort()`s, so a violation under those binaries is a
crash regardless of `NDEBUG` (libFuzzer needs SIGABRT to detect
assertion failures). Tests rely on the debug-build trap.

Sites:

- `next_message_reader()` on an incomplete reader.
- `calculate_check_sum()` / `message_end()` / `begin()` / `end()` /
  `message_type()` / `check_sum()` on an invalid reader.
- `iterator::operator+(n)` with `n < 0`.

These are caller-contract checks, not wire-data checks. A
well-formed call site will not fire them no matter what bytes arrive
on the socket.

### Custom handler

Define `HFFIX_ASSERT` before `<hffix.hpp>`:

```cpp
extern void hffix_on_assert(char const* msg) noexcept;

#define HFFIX_ASSERT(cond, msg) \
    do { if (!(cond)) [[unlikely]] hffix_on_assert(msg); } while (0)

#include <hffix.hpp>
```

The library's definition is gated on `#ifndef HFFIX_ASSERT`, so the
override wins. In a monorepo, set it once in a project-wide header
that precedes `<hffix.hpp>` in every TU.

## Timestamp range

Epoch-materializing parsers reject `year < 1970` or `year > 2200`:

- `field_value::as_epoch_nanos()` → `std::optional<int64_t>`
- `field_value::as_epoch_millis()` → `std::optional<int64_t>`
- `field_value::as_timestamp(std::chrono::time_point&)`
- `field_value::as_timestamp_nano(std::chrono::time_point&)`

Out-of-range `YYYY` returns `nullopt` / `false`. The int-out
overloads (`as_timestamp(int& y, int& m, ...)`, `as_date`,
`as_timeonly`, `as_timeonly_nano`) skip epoch math and are
unaffected.

### Why

FIX `YYYY` is 4 ASCII digits, so wire input can carry `0000..9999`.
For `year = 9999`, building a `time_point<_, nanoseconds>` triggers
a `seconds → nanoseconds` widening (`* 1'000'000'000`). Result
overflows `int64_t` and wraps silently; the `time_point` holds
garbage.

`<chrono>` does not check arithmetic overflow. `duration_cast`,
`operator+` on durations, and the `common_type` promotion that fires
on `seconds + nanoseconds` are plain `int64_t` ops with wrap on
overflow. The real `chrono::sys_time<nanoseconds>` wall is
`2262-04-11 23:47:16.854775807 UTC` (known "year 2262 problem"). Cap
2200 sits below it with margin for the proleptic-Gregorian
day-of-year / era-boundary corner cases.

### Bypass

For dates outside `[1970, 2200]`, use the int-out `as_timestamp`
overload. It skips the epoch path and bounds only
`month ∈ [1, 12]`, `day ∈ [1, 31]`. Caller handles date arithmetic.

## Thread safety

- `basic_message_reader` is **logically immutable** after construction.
  Concurrent reads from multiple threads on the same `const&` are safe.
  Copies are independent (the copy constructor re-runs `init()`),
  cheap, and safe to make across threads.
- `basic_message_reader_const_iterator` references its parent reader
  and the underlying buffer. Iterators dereferenced concurrently on
  the same buffer are safe; do not move them across threads while
  one of them is being advanced.
- `basic_indexed_message` is read-only after `build_field_index`
  returns. Concurrent `find_with_hint` / `has` / `value_at` calls on
  the same instance are safe. `find_with_hint` mutates the
  caller-supplied `hint`, so do not share the same hint variable
  across threads.
- `message_writer` is **single-owner**. Do not share an instance
  across threads or invoke `push_back_*` concurrently on the same
  writer. The buffer the writer points into must not be read by
  another thread until `push_back_trailer()` returns `true`.
- The library uses one global, `constinit` lookup table
  (`details::g_length_tag_table`). Static-init order is safe; reads
  are concurrent-clean.
- No file-scope state mutates at runtime; no hidden allocations; no
  thread-local state.

The de-facto deployment model is one reader/writer per worker thread,
each operating on a per-thread buffer fed from a single feed handler.
Sharing a *parsed* `indexed_message` to fan out across consumer
threads is supported as long as the underlying buffer is not mutated.

## Batched parse

A single TCP `recv()` or Aeron poll typically delivers N concatenated
FIX frames. Two helpers iterate them without manual cursor bookkeeping:

```cpp
#include <hffix.hpp>

// Range form. Yields complete + valid readers; skips invalid frames via
// next_message_reader's "8=FIX" resync; stops at the first incomplete
// frame.
void handle(char const* buf, std::size_t n) {
    auto range = hffix::messages(buf, n);
    auto it = range.begin();
    for (; it != range.end(); ++it) {
        process(*it);
    }
    char const* tail_begin = it.remainder(); // partial frame to keep
    // copy [tail_begin, buf + n) into the head of the next read buffer
}
```

```cpp
// Callback form. Returns the unconsumed tail pointer directly.
char const* tail = hffix::for_each_message(buf, buf + n,
    [&](hffix::message_reader const& r) {
        process(r);
    });
```

Both forms prefetch the next message's header before invoking the
callback. Per-message overhead is the same as a hand-rolled
`while (r.is_complete()) { ... r = r.next_message_reader(); }` loop;
the helpers just hide the `is_complete` / `is_valid` ladder and the
tail-pointer extraction.

Pick the range form when you want to compose with `std::ranges` /
algorithms or break out of iteration. Pick the callback form when you
have a single dispatch closure and want the tail pointer in one
expression.

## Repeating groups

`reader.group<tag::NoXxx>()` returns a `basic_group_view` over the
group's entries. The delimiter tag (first tag of the entry block) is
looked up at compile time in `hffix::groups::group_def<CountTag>`:

```cpp
hffix::message_reader r(buf, w.message_end());
if (!r.is_valid()) return;

for (auto const& entry : r.group<hffix::tag::NoMDEntries>()) {
    auto it = entry.begin();
    if (entry.find_with_hint(hffix::tag::MDEntryPx, it))
        auto px = it->value();
    if (entry.find_with_hint(hffix::tag::MDEntrySize, it))
        auto sz = it->value().as_int_unchecked<int>();
}
```

All FIX 5.0 SP2 + FIXT 1.1 groups are pre-registered via the generated
`hffix_groups.hpp`. To override a delimiter (for venue extensions or a
group whose delimiter differs across messages), declare your own
registration at namespace scope:

```cpp
HFFIX_REGISTER_GROUP(NoQuoteEntries, QuoteEntryID);
```

Unknown `CountTag` triggers a `static_assert`. The runtime overload
`reader.group(count_tag, first_tag)` is also available for ad-hoc
groups without registration.

The iterator is bounded by the NoXxx count; trailing non-group fields
do not leak into the last entry. Nested groups: call
`entry.group<Inner>()` on a `basic_group_entry`.

For many lookups per entry, build a per-entry index:

```cpp
hffix::field_index_buffer<32> entry_idx;
for (auto const& entry : r.group<hffix::tag::NoMDEntries>()) {
    auto idx = hffix::build_field_index(entry, entry_idx);
    std::size_t h = 0;
    auto px = idx.find_with_hint(hffix::tag::MDEntryPx, h);
    // ...
}
```

## Fuzzing

Standalone CMake project under [fuzz/](fuzz/) builds a libFuzzer + ASan +
UBSan binary that hammers `message_reader`, `for_each_message`,
repeating groups, `build_field_index`, and the `try_as_*` parser
family.

The `fuzz` target depends on `fuzz_dataset`, which copies seed files
from [tests/data/](tests/data/) into `build/fuzz/dataset/` before the
run. Mutated inputs that libFuzzer persists land in the same build-tree
directory. Crashes / timeouts are written next
to the working directory; replay with
`build/fuzz/fuzz_reader ./crash-<hash>`.

## Build options

| Option | Default | Effect |
| --- | --- | --- |
| `HFFIX_BUILD` | ON top-level / OFF as subdir | Tests, benchmarks, CLI. |
| `HFFIX_BUILD_DOCS` | OFF | Doxygen HTML (requires `doxygen` in PATH). |
| `HFFIX_BUILD_FIXSPEC_GEN` | matches `HFFIX_BUILD` | Builds `fixspec-gen-fields`. |
| `HFFIX_NATIVE_ARCH` | ON | `-mcpu=native` / `-march=native`. |
| `HFFIX_LTO` | ON | `CMAKE_INTERPROCEDURAL_OPTIMIZATION` (Clang/AppleClang forced to `-flto=full`). |
| `HFFIX_SANITIZE` | OFF | `-fsanitize=address,undefined`. Use with `-DCMAKE_BUILD_TYPE=Debug` and `-DHFFIX_LTO=OFF`. |
| `HFFIX_SANITIZE_THREAD` | OFF | `-fsanitize=thread`. Mutually exclusive with `HFFIX_SANITIZE`. |
| `HFFIX_BENCH_MESSAGES` | 500000 | Dataset size for `bench_data`. |
| `HFFIX_BENCH_MIN_TIME` | `1s` | `--benchmark_min_time` for `bench`. |
| `HFFIX_BENCH_REPETITIONS` | 5 | `--benchmark_repetitions` for `bench`. |

Debug + sanitizers:

```sh
conan install . --output-folder=build-asan --build=missing \
    -s build_type=Debug -s compiler.cppstd=gnu20 -s:b compiler.cppstd=gnu20
cmake --preset conan-debug -DHFFIX_SANITIZE=ON -DHFFIX_LTO=OFF -DHFFIX_NATIVE_ARCH=OFF
cmake --build build-asan -j
ctest --test-dir build-asan
```

Swap `-DHFFIX_SANITIZE=ON` for `-DHFFIX_SANITIZE_THREAD=ON` for TSan.
`HFFIX_SANITIZE` and `HFFIX_SANITIZE_THREAD` are mutually exclusive.

## Regenerating `hffix_fields.hpp` and `hffix_groups.hpp`

Source: QuickFIX-format XML specs (`fixspec/FIX50SP2.xml` +
`fixspec/FIXT11.xml`). Single tool invocation emits both headers:

```sh
cmake --build build --target fixspec-gen-fields
build/utils/fixspec-gen-fields fixspec/FIX50SP2.xml fixspec/FIXT11.xml \
    -o include/hffix_fields.hpp \
    -go include/hffix_groups.hpp
```

FIX 5.0 splits session (FIXT.1.1) from application (FIX.5.0 SP2) into
two files; pass both. Extra venue-specific XMLs can be appended as
further positional args; fields/messages with identical definitions
are deduplicated, name conflicts on the same tag emit aliases, group
delimiters that differ across messages keep the first-encountered
one and the rest stay reachable via the runtime overload
`reader.group(count_tag, first_tag)`.

`hffix_groups.hpp` carries `HFFIX_REGISTER_GROUP(NoXxx, FirstTag);`
declarations for every `<group>` in the input specs (~500 for
FIX 5.0 SP2 + FIXT 1.1). The bundled copy is committed; regenerate
to track a spec update or pull in a venue extension.

Downstream projects that consume hffix via `add_subdirectory(hffix)` or
`find_package(hffix)` can call the bundled CMake helper:

```cmake
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE hffix2::hffix)

hffix_generate_fields(
    TARGET   my_app
    SPEC_XML ${PROJECT_SOURCE_DIR}/my_spec/FIX50SP2.xml
             ${PROJECT_SOURCE_DIR}/my_spec/FIXT11.xml
)
```

The helper builds the generator if needed, regenerates
`hffix_fields.hpp` and `hffix_groups.hpp` from the supplied specs,
adds the dependency to `my_app`, and prepends the output dir to its
include path with `BEFORE` so the regenerated headers shadow the
bundled ones.

