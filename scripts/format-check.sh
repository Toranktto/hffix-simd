#!/bin/sh
# clang-format dry-run; exit non-zero if any file would change.
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
CLANG_FORMAT=${CLANG_FORMAT:-clang-format}

if ! command -v "$CLANG_FORMAT" >/dev/null 2>&1; then
    echo "error: $CLANG_FORMAT not found (set CLANG_FORMAT)" >&2
    exit 1
fi

"$SCRIPT_DIR/_sources.sh" | xargs "$CLANG_FORMAT" --dry-run --Werror
