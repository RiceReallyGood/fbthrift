#!/usr/bin/env bash
# Check out on suzhou950:~/cluster_aware/src exactly the commits checked out
# locally and record them in ~/cluster_aware/SOURCE_VERSIONS. Run locally.
#
# suzhou950 fetches from GitHub through its HTTP proxy (git config --global
# http.https://github.com/.proxy): folly / fbthrift from the forks, which must
# already hold the local HEADs (this script never pushes), fizz / wangle from
# upstream. Uncommitted local changes would not reach suzhou950, so the script
# refuses to run with any. fbthrift-tmfix (branch cluster-aware-tmfix) must
# contain fbthrift-ca's HEAD (merge cluster-aware-bench into it) and differ
# from it only in the TMFIX files, so that the two fbthrift builds differ in
# nothing else.
set -euo pipefail

ROOT=${ROOT:-/home/fanzhenhao/fbthrift+folly}
HOST=${HOST:-suzhou950}
REMOTE=cluster_aware

# <dir under src/> <local checkout under $ROOT> <GitHub repo> <local remote
# that must already contain HEAD, or - for upstream tags>
REPOS=(
  "folly folly-ca RiceReallyGood/folly fork"
  "fbthrift fbthrift-ca RiceReallyGood/fbthrift fork"
  "fbthrift-tmfix fbthrift-tmfix RiceReallyGood/fbthrift fork"
  "fizz third_party_src/fizz facebookincubator/fizz -"
  "wangle third_party_src/wangle facebook/wangle -"
)

TMFIX_FILES="thrift/lib/cpp2/async/AsyncProcessor.cpp
thrift/lib/cpp2/async/AsyncProcessor.h"
bench_head=$(git -C "$ROOT/fbthrift-ca" rev-parse HEAD)
if ! git -C "$ROOT/fbthrift-tmfix" merge-base --is-ancestor "$bench_head" HEAD ||
   [ "$(git -C "$ROOT/fbthrift-tmfix" diff --name-only "$bench_head" HEAD)" != \
     "$TMFIX_FILES" ]; then
  echo "fbthrift-tmfix must contain fbthrift-ca's HEAD and differ from it only" \
    "in the TMFIX files (git -C $ROOT/fbthrift-tmfix merge cluster-aware-bench)" >&2
  exit 1
fi

plan=()
for entry in "${REPOS[@]}"; do
  read -r name dir repo remote <<<"$entry"
  if [ -n "$(git -C "$ROOT/$dir" status --porcelain)" ]; then
    echo "$dir has uncommitted changes; commit them first" >&2
    exit 1
  fi
  sha=$(git -C "$ROOT/$dir" rev-parse HEAD)
  if [ "$remote" != - ] && [ -z "$(git -C "$ROOT/$dir" for-each-ref \
      --contains "$sha" "refs/remotes/$remote")" ]; then
    echo "$dir HEAD $sha is not on $remote; push it first" \
      "(git -C $ROOT/$dir push $remote $(git -C "$ROOT/$dir" branch --show-current))" >&2
    exit 1
  fi
  plan+=("$name,https://github.com/$repo.git,$sha")
done

# shellcheck disable=SC2029
ssh "$HOST" REMOTE="$REMOTE" bash -s -- "${plan[@]}" <<'EOF'
set -euo pipefail
src=~/$REMOTE/src
versions=()
for entry in "$@"; do
  IFS=, read -r name url sha <<<"$entry"
  dir=$src/$name
  if [ ! -e "$dir" ]; then
    git init -q "$dir"
    git -C "$dir" remote add origin "$url"
  fi
  [ -d "$dir/.git" ] || { echo "$dir is not a git checkout" >&2; exit 1; }
  git -C "$dir" fetch -q --depth=50 origin "$sha"
  git -C "$dir" checkout -q --detach "$sha"
  versions+=("$name $sha dirty_files=$(git -C "$dir" status --porcelain | wc -l)")
done
printf '%s\n' "${versions[@]}" | tee ~/$REMOTE/SOURCE_VERSIONS
EOF

echo "synced to $HOST:~/$REMOTE/src"
