#!/usr/bin/env bash
# Build the baseline dependency chain, the press benchmark and the
# cluster-aware folly tests on suzhou950.
#
# Usage: build.sh folly|fizz|wangle|fbthrift|bench|all
#        build.sh ca_test|evb_test [gtest args...]
#        build.sh bench_test [ctest args...]
# Env overrides (e.g. for the vanilla-EventBase or TMFIX comparison builds, or
# the dev trees of dev_build.sh):
#   INS=<install prefix>  FOLLY_SRC=<folly source>
#   FBTHRIFT_SRC=<fbthrift source, also the benchmark's>  SUFFIX=<build dir suffix>
#
# Refuses to build while this user runs an experiment (builds would disturb it).
set -euo pipefail

if pgrep -u "$(id -u)" -f 'stage1\.py|stage2\.py|run_point\.sh|run_v2\.sh' >/dev/null; then
  echo "experiment running; not building" >&2
  exit 1
fi

CA=${CA:-$HOME/cluster_aware}
SRC=$CA/src
BLD=$CA/build
INS=${INS:-$CA/ins}
FOLLY_SRC=${FOLLY_SRC:-$SRC/folly}
FBTHRIFT_SRC=${FBTHRIFT_SRC:-$SRC/fbthrift}
SUFFIX=${SUFFIX:-}
JOBS=${JOBS:-128}

COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release
  "-DCMAKE_INSTALL_PREFIX=$INS" "-DCMAKE_PREFIX_PATH=$INS"
  -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  # openEuler leaves this empty and folly then assumes x86_64 (adds -mavx etc.).
  "-DCMAKE_LIBRARY_ARCHITECTURE=$(uname -m)-linux-gnu")

# Always re-run configure: folly collects sources by glob, so new files are
# only picked up by a fresh configure.
lib() {
  local name=$1 src=$2
  shift 2
  cmake -S "$src" -B "$BLD/$name$SUFFIX" "${COMMON[@]}" "$@"
  cmake --build "$BLD/$name$SUFFIX" --target install -j "$JOBS"
}

bench() {
  local dir=$FBTHRIFT_SRC/fbthrift_folly_benchmark
  (cd "$dir" && "$INS/bin/thrift1" --gen mstch_cpp2 -o . press.thrift)
  cmake -S "$dir" -B "$BLD/bench$SUFFIX" "${COMMON[@]}"
  cmake --build "$BLD/bench$SUFFIX" -j "$JOBS"
}

bench_test() {
  bench
  (cd "$BLD/bench$SUFFIX" && ctest --output-on-failure "$@")
}

evb_test() {
  cmake -S "$FOLLY_SRC/folly/io/async/test/evb_wakeup" -B "$BLD/evb_test$SUFFIX" "${COMMON[@]}"
  cmake --build "$BLD/evb_test$SUFFIX" -j "$JOBS"
  (cd "$BLD/evb_test$SUFFIX" && ./evb_wakeup_test "$@")
}

ca_test() {
  cmake -S "$FOLLY_SRC/folly/executors/test/cluster_aware" -B "$BLD/ca_test$SUFFIX" "${COMMON[@]}"
  cmake --build "$BLD/ca_test$SUFFIX" -j "$JOBS"
  (cd "$BLD/ca_test$SUFFIX" && ./cluster_aware_test "$@")
}

target=${1:-}
[ $# -gt 0 ] && shift
case $target in
  folly) lib folly "$FOLLY_SRC" -DBUILD_TESTS=OFF ;;
  fizz) lib fizz "$SRC/fizz/fizz" -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF ;;
  wangle) lib wangle "$SRC/wangle/wangle" -DBUILD_TESTS=OFF ;;
  fbthrift) lib fbthrift "$FBTHRIFT_SRC" -Denable_tests=OFF ;;
  bench) bench ;;
  bench_test) bench_test "$@" ;;
  ca_test) ca_test "$@" ;;
  evb_test) evb_test "$@" ;;
  all)
    for t in folly fizz wangle fbthrift; do "$0" "$t"; done
    bench
    ;;
  *)
    echo "usage: $0 folly|fizz|wangle|fbthrift|bench|all|ca_test|evb_test|bench_test [test args]" >&2
    exit 2
    ;;
esac
