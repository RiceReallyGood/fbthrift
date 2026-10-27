#!/usr/bin/env bash
# Build the baseline dependency chain, the press benchmark and the
# cluster-aware folly tests on suzhou950.
#
# Usage: build.sh folly|fizz|wangle|fbthrift|bench|all
#        build.sh ca_test [gtest args...]
set -euo pipefail

CA=${CA:-$HOME/cluster_aware}
SRC=$CA/src
BLD=$CA/build
INS=$CA/ins
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
  cmake -S "$src" -B "$BLD/$name" "${COMMON[@]}" "$@"
  cmake --build "$BLD/$name" --target install -j "$JOBS"
}

bench() {
  local dir=$SRC/fbthrift/fbthrift_folly_benchmark
  (cd "$dir" && "$INS/bin/thrift1" --gen mstch_cpp2 -o . press.thrift)
  cmake -S "$dir" -B "$BLD/bench" "${COMMON[@]}"
  cmake --build "$BLD/bench" -j "$JOBS"
}

ca_test() {
  cmake -S "$SRC/folly/folly/executors/test/cluster_aware" -B "$BLD/ca_test" "${COMMON[@]}"
  cmake --build "$BLD/ca_test" -j "$JOBS"
  (cd "$BLD/ca_test" && ./cluster_aware_test "$@")
}

target=${1:-}
[ $# -gt 0 ] && shift
case $target in
  folly) lib folly "$SRC/folly" -DBUILD_TESTS=OFF ;;
  fizz) lib fizz "$SRC/fizz/fizz" -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF ;;
  wangle) lib wangle "$SRC/wangle/wangle" -DBUILD_TESTS=OFF ;;
  fbthrift) lib fbthrift "$SRC/fbthrift" -Denable_tests=OFF ;;
  bench) bench ;;
  ca_test) ca_test "$@" ;;
  all)
    for t in folly fizz wangle fbthrift; do "$0" "$t"; done
    bench
    ;;
  *)
    echo "usage: $0 folly|fizz|wangle|fbthrift|bench|all|ca_test [gtest args]" >&2
    exit 2
    ;;
esac
