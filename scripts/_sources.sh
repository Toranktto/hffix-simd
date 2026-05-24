#!/bin/sh
# Print the C++ files we run clang-format over. Excludes live in .clang-format-ignore.
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
cd "$SCRIPT_DIR/.."

find include utils tests benchmarks fuzz upstream-benchmarks \
    -type f \( -name '*.hpp' -o -name '*.cpp' \)
