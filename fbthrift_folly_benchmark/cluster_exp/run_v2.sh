#!/usr/bin/env bash
# Scheduler v2 run on suzhou950 (all results of the v2 report): B0, CA, CA2,
# CA2-G, ST, ST-P (and EB in the main scenarios) on the main build; B0, CA,
# CA2, CA2-G again on the TMFIX build. Run from the synced tree, e.g.
#   nohup ./run_v2.sh > ~/cluster_aware/results/run_v2_driver.log 2>&1 &
set -uo pipefail

CA=${CA:-$HOME/cluster_aware}
EXP=$(cd "$(dirname "$0")" && pwd)
R=${R:-$CA/results/run_v2_$(date +%Y%m%d)}
TMBIN=$CA/build/bench-tmfix

MAIN=B0,CA,CA2,CA2-G,ST,ST-P,EB
SWEEP=B0,CA,CA2,CA2-G,ST,ST-P
TM=B0,CA,CA2,CA2-G
S1_LOADS=50000,100000,200000,300000,450000,630000
S2_LOADS=100000,200000,400000,600000,900000,1260000
S2_SWEEP_LOADS=100000,200000,400000,600000,900000
IMB_LOADS=25000,50000,100000,150000

# Each build must link its own installs: HandlerCallbackBase differs between
# the fbthrift with and without TMFIX, so mixing them is undefined.
check_build() {
  local dir=$1 prefix=$2 var
  if [ ! -x "$dir/press_server" ] || [ ! -x "$dir/press_client" ]; then
    echo "missing $dir/press_server or press_client" >&2
    exit 1
  fi
  for var in FBThrift_DIR folly_DIR; do
    if ! grep -q "^$var:PATH=$prefix/" "$dir/CMakeCache.txt"; then
      echo "$dir: $var is not under $prefix/" >&2
      exit 1
    fi
  done
}
check_build "$CA/build/bench" "$CA/ins"
check_build "$TMBIN" "$CA/ins-tmfix"

mkdir -p "$R"
cd "$EXP"
cp "$CA/SOURCE_VERSIONS" "$R/"

run() {
  local name=$1
  shift
  echo "$(date '+%F %T') start $name"
  python3 "$@" --out "$R/$name" > "$R/$name.log" 2>&1
  echo "$(date '+%F %T') end $name rc=$?"
}

main() { # name scenario configs loads [extra args]
  local name=$1 sc=$2 configs=$3 loads=$4
  shift 4
  run "$name" stage2.py --scenario "$sc" --only-main --configs "$configs" \
    --reps 2 --seconds 20 --abs-loads "$loads" "$@"
}

spe() { # name scenario configs loads [extra args]
  local name=$1 sc=$2 configs=$3 loads=$4
  shift 4
  run "$name" stage2.py --scenario "$sc" --only-main --configs "$configs" \
    --reps 2 --seconds 10 --abs-loads "$loads" --spe-period 65536 "$@"
}

sweep() { # name scenario parts loads
  run "$1" stage2.py --scenario "$2" --parts "$3" --configs "$SWEEP" \
    --reps 2 --seconds 20 --abs-loads "$4"
}

main S1 S1 "$MAIN" "$S1_LOADS"
main S2 S2 "$MAIN" "$S2_LOADS"
spe S1spe S1 "$MAIN" 200000,450000
spe S2spe S2 "$MAIN" 200000,600000
main S1tm S1 "$TM" "$S1_LOADS" --bin-dir "$TMBIN"
main S2tm S2 "$TM" "$S2_LOADS" --bin-dir "$TMBIN"
spe S1spe_tm S1 "$TM" 200000,450000 --bin-dir "$TMBIN"
spe S2spe_tm S2 "$TM" 200000,600000 --bin-dir "$TMBIN"
sweep S1sweep S1 payload,ratio "$S1_LOADS"
sweep S1imb S1 imbalance "$IMB_LOADS"
sweep S2sweep S2 payload,ratio "$S2_SWEEP_LOADS"
sweep S2imb S2 imbalance "$IMB_LOADS"
echo "$(date '+%F %T') ALL_DONE"
