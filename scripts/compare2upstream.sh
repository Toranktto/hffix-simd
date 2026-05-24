#!/usr/bin/env bash
# Bench N configurations and render comparison tables.
# Env: HFFIX_UPSTREAM_URL, HFFIX_UPSTREAM_REF, HFFIX_BENCH_MIN_TIME (1s),
#      HFFIX_BENCH_REPETITIONS (5),
#      COMPARE2UPSTREAM_DIR (default: <repo>/compare2upstream),
#      PYTHON (default: python3), CONAN (default: conan).

set -euo pipefail

ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)

BASE=${COMPARE2UPSTREAM_DIR:-${ROOT}/compare2upstream}
BUILD=${BASE}/build
UPSTREAM_SRC=${BASE}/upstream-src
RESULTS_DIR=${BASE}/results
CONAN_DIR=${BUILD}/conan-base
CONAN_TC=${CONAN_DIR}/conan_toolchain.cmake

PYTHON_BIN=${PYTHON:-python3}
CONAN_BIN=${CONAN:-conan}

UPSTREAM_URL=${HFFIX_UPSTREAM_URL:-https://github.com/jamesdbrock/hffix.git}
UPSTREAM_REF=${HFFIX_UPSTREAM_REF:-}
MIN_TIME=${HFFIX_BENCH_MIN_TIME:-1s}
REPS=${HFFIX_BENCH_REPETITIONS:-5}
DATA_DIR=${ROOT}/benchmarks/data
mkdir -p "${BUILD}" "${RESULTS_DIR}"

if [[ ! -f ${CONAN_TC} ]]; then
    if ! command -v "${CONAN_BIN}" > /dev/null; then
        echo "error: '${CONAN_BIN}' not in PATH; install Conan 2, set CONAN=<path>, or set COMPARE2UPSTREAM_DIR to a tree that already contains build/conan-base/conan_toolchain.cmake" >&2
        exit 1
    fi
    echo ">> running conan install -> ${CONAN_DIR}"
    "${CONAN_BIN}" install "${ROOT}" \
        --output-folder "${CONAN_DIR}" \
        --build=missing \
        --settings=build_type=Release > /dev/null
fi

if [[ ! -d ${UPSTREAM_SRC}/.git ]]; then
    echo ">> cloning ${UPSTREAM_URL} -> ${UPSTREAM_SRC}"
    rm -rf "${UPSTREAM_SRC}"
    if [[ -n ${UPSTREAM_REF} ]]; then
        git clone --depth 1 --branch "${UPSTREAM_REF}" "${UPSTREAM_URL}" "${UPSTREAM_SRC}"
    else
        git clone --depth 1 "${UPSTREAM_URL}" "${UPSTREAM_SRC}"
    fi
fi
UPSTREAM_INC=${UPSTREAM_SRC}/include
[[ -f ${UPSTREAM_INC}/hffix.hpp ]] || { echo "error: ${UPSTREAM_INC}/hffix.hpp missing" >&2; exit 1; }

# Schema: label|source_dir|exec_relpath|extra_cmake_args
CONFIGS=(
    "upstream|${ROOT}/upstream-benchmarks|hffix_upstream_benchmarks|-DHFFIX_UPSTREAM_INCLUDE_DIR=${UPSTREAM_INC}"
    "fork|${ROOT}|benchmarks/hffix_benchmarks|"
)

EXE_SUFFIX=""
[[ ${OS:-} == Windows_NT ]] && EXE_SUFFIX=.exe

ensure_configured() {
    local build_dir=$1
    local source_dir=$2
    local extra=$3
    if [[ ! -f ${build_dir}/CMakeCache.txt ]]; then
        # shellcheck disable=SC2086
        cmake -S "${source_dir}" -B "${build_dir}" -G Ninja \
            -DCMAKE_TOOLCHAIN_FILE="${CONAN_TC}" \
            -DCMAKE_BUILD_TYPE=Release \
            ${extra} > /dev/null
    fi
}

if ! ls "${DATA_DIR}"/*.fix > /dev/null 2>&1; then
    echo ">> generating dataset"
    ensure_configured "${BUILD}/fork" "${ROOT}" ""
    cmake --build "${BUILD}/fork" --target bench_data
fi

BENCH_ARGS=(
    --benchmark_min_time="${MIN_TIME}"
    --benchmark_repetitions="${REPS}"
    --benchmark_report_aggregates_only=true
    --benchmark_enable_random_interleaving=true
    --benchmark_format=json
)

RENDER_ARGS=()
for cfg in "${CONFIGS[@]}"; do
    IFS='|' read -r label source_dir exec_relpath extra <<< "${cfg}"
    build_dir="${BUILD}/${label}"
    target="${exec_relpath##*/}"
    json="${RESULTS_DIR}/${label}.json"

    echo ">> ${label}"
    ensure_configured "${build_dir}" "${source_dir}" "${extra}"
    cmake --build "${build_dir}" --target "${target}" > /dev/null
    "${build_dir}/${exec_relpath}${EXE_SUFFIX}" "${BENCH_ARGS[@]}" > "${json}"
    RENDER_ARGS+=("${label}=${json}")
done

echo
echo "================================================================"
echo "min_time=${MIN_TIME}  repetitions=${REPS}"
echo "================================================================"
echo
"${PYTHON_BIN}" "${ROOT}/scripts/compare2upstream_render.py" "${RENDER_ARGS[@]}"
