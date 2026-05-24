# CLAUDE.md

Project-level guidance for Claude.

## What this is

`hffix2` is a header-only FIX 4.x/5.0 parser. Fork of
`jamesdbrock/hffix`. Scope: serialize + deserialize FIX wire format.
Nothing else. No transport, no session layer, no business logic.

## Hard rules

1. **Parser only.** Do not add session-state, sequence-number tracking,
   reconnect logic, persistence, or any networking. If a feature
   conceptually belongs in QuickFIX/OnixS, it does not belong here.
2. **Zero-alloc end-to-end.** No `std::string`, `std::vector`, `new`,
   `malloc` on any reader or writer path. Buffer is always
   caller-supplied. Views are spans/string_views into that buffer.
3. **No exceptions on hot path.** Reader and writer methods are
   `noexcept`. Errors surface via return code or sticky flag.
   Programmer-error invariants use `HFFIX_ASSERT`. Under `NDEBUG` the
   default impl bumps an atomic counter (`hffix::assert_failure_count()`);
   in debug builds it traps. Benchmarks and the fuzz harness ship
   `assert_override.hpp` that always aborts (libFuzzer needs SIGABRT
   to detect assertion failures under `NDEBUG`). Tests rely on the
   debug trap; override the macro before include for a custom handler.
4. **Header-only.** Everything goes in `include/hffix.hpp` or
   `include/hffix_groups.hpp` (generated). No `.cpp` for library code.
5. **C++20.** Do not gate features on older standards.
6. **No SIMD.** Earlier versions of this library shipped AVX2 / NEON
   scalar-policy machinery. Benchmarks across every architecture
   (x86-64 AVX2, ARM NEON on M1/M4) showed scalar-only consistently
   beating the SIMD path on real FIX traffic — short value spans don't
   amortise SIMD setup cost. The SIMD code was deleted. Do not propose
   re-adding SIMD policies, AVX2/NEON/SVE intrinsics, or a `Policy`
   template parameter to readers/iterators/groups — the current scalar
   path is intentional and faster.

## Hot-path conventions

- Annotate hot functions: `HFFIX_HOT HFFIX_ALWAYS_INLINE`.
- Mark error/rare branches `[[unlikely]]`. Mark predictable common
  branches `[[likely]]` only when profiling justifies it.

## Validating vs unchecked parsers

- `try_as_int(T&)` / `try_as_decimal(T&, T&)`: validating, no overflow
  detection, `noexcept`, returns `bool`.
- `as_int_unchecked<T>()` / `as_decimal_unchecked<T>()`: garbage in =
  undefined output. Suffix is mandatory consent.
- Default new code to the `try_*` variants. Use `_unchecked` only when
  the caller has externally validated the field.

## Build

Toolchain: CMake 3.20+, Conan 2.x, C++20.

```sh
pip install 'conan>=2.0,<3.0'
conan profile detect --force

conan install . --output-folder=build --build=missing \
    -s build_type=Release \
    -s compiler.cppstd=gnu20 -s:b compiler.cppstd=gnu20
cmake --preset conan-release
cmake --build build -j
```

Binaries land in `build/utils/` (`fixprint`, `fixgen`,
`fixspec-gen-fields`) and `build/benchmarks/hffix_benchmarks`.

Useful CMake options (defaults in parens):

| Option | Default | Effect |
| --- | --- | --- |
| `HFFIX_BUILD` | ON top-level | Builds tests, benches, CLI utils. |
| `HFFIX_NATIVE_ARCH` | ON | `-march=native` / `-mcpu=native`. |
| `HFFIX_LTO` | ON | `CMAKE_INTERPROCEDURAL_OPTIMIZATION`; Clang/AppleClang forced to `-flto=full`. |
| `HFFIX_SANITIZE` | OFF | ASan + UBSan. Use with `-DCMAKE_BUILD_TYPE=Debug` and `-DHFFIX_LTO=OFF`. |
| `HFFIX_SANITIZE_THREAD` | OFF | TSan. Mutually exclusive with `HFFIX_SANITIZE`. |
| `HFFIX_BENCH_MESSAGES` | 500000 | Synthetic dataset size for `bench_data`. |
| `HFFIX_BENCH_MIN_TIME` | `1s` | `--benchmark_min_time` for `bench` target. |
| `HFFIX_BENCH_REPETITIONS` | 5 | `--benchmark_repetitions` for `bench` target. |

## Test

```sh
ctest --test-dir build --output-on-failure
```

Tests live in `tests/unit_tests.cpp` (API surface) and
`tests/integration_tests.cpp` (round-trip + malformed-input fixtures
from `tests/data/`). Both run as a single `tests` binary via
GoogleTest. Run after every header change.

## Sanitizers

```sh
conan install . --output-folder=build-asan --build=missing \
    -s build_type=Debug -s compiler.cppstd=gnu20 -s:b compiler.cppstd=gnu20
cmake --preset conan-debug -DHFFIX_SANITIZE=ON -DHFFIX_LTO=OFF -DHFFIX_NATIVE_ARCH=OFF
cmake --build build-asan -j
ctest --test-dir build-asan
```

Use ASan/UBSan whenever touching reader state machines (`init()`,
`increment()`, `next_message_reader()`) or pointer arithmetic guards.

## Synthetic dataset (`benchmarks/data/synthetic.fix`)

The dataset is NOT committed (multi-GB at default size). Generate
locally:

```sh
cmake --build build --target bench_data
```

Writes `benchmarks/data/synthetic.fix` with `HFFIX_BENCH_MESSAGES`
messages (default 500000) using `utils/fixgen`. `fixgen` is
deterministic: seeded `std::mt19937` (default seed 42 via `-s`),
fixed message-type mix (MD snapshots, MD incrementals,
NewOrderSingle, ExecutionReport, Logon/Logout/Heartbeat).

Re-run `bench_data` only when:
- the generator code changed,
- `HFFIX_BENCH_MESSAGES` changed,
- the seed changed,
- spec headers were regenerated and field offsets shifted.

Otherwise the existing file is reused across bench runs.

Direct invocation if needed:
```sh
build/utils/fixgen -o benchmarks/data/synthetic.fix -n 500000 -s 42
```

## Benchmarks

```sh
cmake --build build --target bench
```

`bench` does NOT depend on `bench_data`; missing dataset = the bench
emits a skip. Bench parameters come from cache vars
(`HFFIX_BENCH_MIN_TIME`, `HFFIX_BENCH_REPETITIONS`). The binary is
`build/benchmarks/hffix_benchmarks`; invoke directly for custom
Google Benchmark flags.

## Compare against upstream

Differential bench vs `jamesdbrock/hffix`:

```sh
scripts/compare2upstream.sh
```

What it does:
1. Clones upstream to `compare2upstream/upstream-src/` (cached).
   Pin via `HFFIX_UPSTREAM_REF=<tag>` env.
2. Generates `benchmarks/data/synthetic.fix` if missing.
3. Builds two configs into `compare2upstream/build/{upstream,fork}/`:
   - `upstream`: standalone CMake project at `upstream-benchmarks/`
     building `hffix-upstream-benchmarks` against
     `upstream-benchmarks/upstream_benchmarks.cpp` (only uses upstream
     API surface). Configured with `-DHFFIX_UPSTREAM_INCLUDE_DIR=...`
     pointing at the cloned upstream-src include dir.
   - `fork`: this repo.
4. Runs each binary with the same `--benchmark_*` args, writes JSON
   to `compare2upstream/results/`.
5. Renders side-by-side tables via
   `scripts/compare2upstream_render.py`.

Override URL / ref / timing via env: `HFFIX_UPSTREAM_URL`,
`HFFIX_UPSTREAM_REF`, `HFFIX_BENCH_MIN_TIME`,
`HFFIX_BENCH_REPETITIONS`.

`upstream-benchmarks/upstream_benchmarks.cpp` is intentionally limited
to upstream's API (no `try_as_int`, no indexed message). It lives in a
standalone CMake project (`upstream-benchmarks/`) that does NOT depend
on the main repo's CMakeLists; it links against upstream's headers via
`HFFIX_UPSTREAM_INCLUDE_DIR`. If a bench needs new fork API, add it
ONLY to `benchmarks/benchmarks.cpp`.

## Generated headers

- `include/hffix_fields.hpp` (~12500 lines): do NOT edit by hand.
- `include/hffix_groups.hpp` (~650 lines): do NOT edit by hand.

Regenerate after any spec change:

```sh
cmake --build build --target fixspec-gen-fields
build/utils/fixspec-gen-fields fixspec/FIX50SP2.xml fixspec/FIXT11.xml \
    -o include/hffix_fields.hpp \
    -go include/hffix_groups.hpp
```

FIX 5.0 needs both inputs (`FIX50SP2.xml` for app messages,
`FIXT11.xml` for session). Extra venue specs append as more positional
args; conflicts on the same tag resolve as first-wins for delimiters,
alias for name clashes. Commit XML + generated headers together.

Downstream projects can regenerate at build time via
`hffix_generate_fields(TARGET ... SPEC_XML ...)` in `CMakeLists.txt`.

## Formatting

```sh
scripts/format.sh        # rewrite in place
scripts/format-check.sh  # dry-run, exits non-zero on diff (CI)
```

Both honor `.clang-format-ignore`. `_sources.sh` enumerates the file
list (`include/`, `utils/`, `tests/`, `benchmarks/`); do not run
`clang-format` directly on `compare2upstream/` or the generated
headers.

## Tests are part of the API contract

Every public API change must update `tests/unit_tests.cpp`. Bounds
behavior changes go in `tests/integration_tests.cpp`. If a test fails
after a change, the change is wrong by default; do not loosen the test
to make it pass without understanding why it failed.

## Style

- `.clang-format` is authoritative. Run `scripts/format.sh` before
  committing.
- Comments: only where the "why" is non-obvious. Do not narrate the
  code. No decorative separators (`// === foo ===`).
- Doxygen blocks only on public types/methods, and only when they say
  something the signature does not.

## What not to do

- Do not add Boost. Boost.DateTime was removed intentionally; `chrono`
  + raw epoch ints are the only time API.
- Do not add `as_string()` back (returned `std::string`, allocated).
- Do not throw from any reader or writer method.
- Do not silently expand `field_index_buffer<N>` capacity. Caller
  controls `N`; overflow is a documented failure mode
  (`truncated()` / `overflowed()`).
- Do not edit `compare2upstream/upstream-src/`; that tree is a
  pinned snapshot of upstream for differential benchmarking.

## Fuzzing

`fuzz/` is a standalone CMake project. Apple Clang lacks
`libclang_rt.fuzzer`; use Homebrew LLVM:

```sh
cmake -S fuzz -B build/fuzz \
    -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++ \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/fuzz --target fuzz
```

`HFFIX_FUZZ_MAX_TIME=<sec>` overrides the default 60s budget. The
`fuzz` target depends on `fuzz_dataset`, which copies `tests/data/*`
into `build/fuzz/dataset/` at build time. libFuzzer mutates seeds and
persists new coverage-relevant inputs into the same directory; the
build tree is gitignored so nothing leaks into the repo. Crashes are
written next to the working directory; replay with
`build/fuzz/fuzz_reader ./crash-<hash>`.

Add a regression test in `tests/unit_tests.cpp` for every crash the
fuzzer finds, then re-run the harness until the dataset survives at
least one full budget without hits.
