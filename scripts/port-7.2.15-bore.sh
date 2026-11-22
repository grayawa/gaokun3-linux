#!/usr/bin/env bash
# Port the complete gaokun3/kernel 7.2.15-bore tree into the local 7.2.14 tree.
# This deliberately uses a synthetic three-way merge base: v7.2.14.
# It never modifies the destination's gaokun3 branch.
set -Eeuo pipefail

DEST_BRANCH="gaokun3"
PORT_BRANCH="port/7.2.15-bore"
STABLE_TAG="v7.2.14"
UPSTREAM_SHA="206f40c6879ad336544e3dbb1131b34cd355097f"
REPORT_DIR="${REPORT_DIR:-port-report}"
mkdir -p "$REPORT_DIR"

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
say() { printf '\n==> %s\n' "$*" >&2; }

git rev-parse --is-inside-work-tree >/dev/null || die "Run in a git repository"
git remote get-url origin >/dev/null || die "Missing origin remote"

# Clone/fetch with blob filtering: source and destination are both large kernels.
say "Fetch destination $DEST_BRANCH"
git -c protocol.version=2 fetch --no-tags --filter=blob:none --depth=1 origin \
  "refs/heads/$DEST_BRANCH:refs/remotes/origin/$DEST_BRANCH"
DEST_SHA="$(git rev-parse "refs/remotes/origin/$DEST_BRANCH^{commit}")"

if ! git remote get-url gaokun3-bore >/dev/null 2>&1; then
  git remote add gaokun3-bore https://github.com/gaokun3/kernel.git
fi
say "Fetch pinned upstream 7.2.15-bore: $UPSTREAM_SHA"
git -c protocol.version=2 fetch --no-tags --filter=blob:none --depth=1 gaokun3-bore "$UPSTREAM_SHA"
SOURCE_SHA="$(git rev-parse 'FETCH_HEAD^{commit}')"
[[ "$SOURCE_SHA" == "$UPSTREAM_SHA" ]] || die "Unexpected source commit"

if ! git remote get-url stable-rolling >/dev/null 2>&1; then
  git remote add stable-rolling https://github.com/gregkh/linux.git
fi
say "Fetch clean Linux stable base $STABLE_TAG"
git -c protocol.version=2 fetch --no-tags --filter=blob:none --depth=1 stable-rolling \
  "refs/tags/$STABLE_TAG:refs/tags/port-base-$STABLE_TAG"
BASE_SHA="$(git rev-parse "refs/tags/port-base-$STABLE_TAG^{commit}")"

printf 'dest=%s\nupstream=%s\nmerge_base=%s\n' \
  "$DEST_SHA" "$SOURCE_SHA" "$BASE_SHA" > "$REPORT_DIR/refs.txt"

git show "$DEST_SHA:Makefile" | grep -qx 'SUBLEVEL = 14' \
  || die "Destination no longer reports Linux 7.2.14"
git show "$SOURCE_SHA:Makefile" | grep -qx 'SUBLEVEL = 15' \
  || die "Source no longer reports Linux 7.2.15"
git show "$BASE_SHA:Makefile" | grep -qx 'SUBLEVEL = 14' \
  || die "Incorrect synthetic merge base"

say "Three-way merge using the exact Linux $STABLE_TAG release as base"
say "Local gaokun3 changes remain in place unless source changes overlap."
if ! git merge-tree --write-tree --merge-base="$BASE_SHA" \
    "$DEST_SHA" "$SOURCE_SHA" > "$REPORT_DIR/merge-tree.log" 2>&1; then
  echo "Merge conflicts detected. No branch has been changed." >&2
  echo "See $REPORT_DIR/merge-tree.log for conflict paths and details." >&2
  tail -n 100 "$REPORT_DIR/merge-tree.log" >&2
  exit 2
fi
MERGED_TREE="$(head -n 1 "$REPORT_DIR/merge-tree.log")"
[[ "$MERGED_TREE" =~ ^[[:xdigit:]]{40,64}$ ]] || die "Invalid merge-tree result"

# Reject an apparently successful merge if it dropped critical functionality.
say "Validate critical source and device features"
git show "$MERGED_TREE:Makefile" | grep -qx 'SUBLEVEL = 15' \
  || die "Merged tree is not Linux 7.2.15"
git show "$MERGED_TREE:init/Kconfig" | grep -q '^config SCHED_BORE$' \
  || die "BORE Kconfig entry missing"
git show "$MERGED_TREE:kernel/sched/fair.c" | grep -q 'CONFIG_SCHED_BORE' \
  || die "BORE scheduler integration missing"
git show "$MERGED_TREE:arch/arm64/configs/gaokun3_defconfig" | grep -qx 'CONFIG_KVM=y' \
  || die "KVM disabled in device defconfig"
git show "$MERGED_TREE:arch/arm64/configs/gaokun3_defconfig" | grep -qx 'CONFIG_QCOM_FASTRPC=m' \
  || die "FastRPC module disabled in device defconfig"
git cat-file -e "$MERGED_TREE:arch/arm64/boot/dts/qcom/sc8280xp-huawei-gaokun3.dts" \
  || die "gaokun3 DT missing"

git diff --stat "$DEST_SHA" "$MERGED_TREE" > "$REPORT_DIR/diffstat.txt"
git diff --name-status "$DEST_SHA" "$MERGED_TREE" > "$REPORT_DIR/changed-files.txt"
printf 'merged_tree=%s\n' "$MERGED_TREE" >> "$REPORT_DIR/refs.txt"

say "Create a single reviewable port commit based on gaokun3"
# Deliberately do NOT add SOURCE_SHA as a commit parent: the source is a
# different fork network and shallow GitHub pushes must not require its history.
PORT_COMMIT="$(
  GIT_AUTHOR_NAME="GitHub Actions" \
  GIT_AUTHOR_EMAIL="41898282+github-actions[bot]@users.noreply.github.com" \
  GIT_COMMITTER_NAME="GitHub Actions" \
  GIT_COMMITTER_EMAIL="41898282+github-actions[bot]@users.noreply.github.com" \
  git -c commit.gpgsign=false commit-tree "$MERGED_TREE" -p "$DEST_SHA" \
    -m "Linux 7.2.15-bore: port gaokun3-radxa-remix onto gaokun3" \
    -m "Merge base: Linux $STABLE_TAG (gregkh/linux)" \
    -m "Source: https://github.com/gaokun3/kernel/tree/7.2.15-bore" \
    -m "Upstream-Commit: $SOURCE_SHA" \
    -m "Assisted by ChatGPT"
)"
printf 'port_commit=%s\n' "$PORT_COMMIT" >> "$REPORT_DIR/refs.txt"
git update-ref "refs/heads/$PORT_BRANCH" "$PORT_COMMIT" \
  "$(git rev-parse --verify "refs/heads/$PORT_BRANCH" 2>/dev/null || printf '%s' '')"

say "Port generated: $PORT_COMMIT"
cat "$REPORT_DIR/refs.txt"
head -n 70 "$REPORT_DIR/diffstat.txt" || true

if [[ "${PUSH_PORT_BRANCH:-false}" == "true" ]]; then
  say "Push review branch $PORT_BRANCH (never force)"
  git push origin "$PORT_COMMIT:refs/heads/$PORT_BRANCH"
else
  say "Local only; use PUSH_PORT_BRANCH=true to push."
fi

if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
  {
    echo '## gaokun3 Linux 7.2.15-bore port'
    echo
    echo "- Base: \`$DEST_SHA\`"
    echo "- Source: \`$SOURCE_SHA\`"
    echo "- Linux stable merge base: \`$BASE_SHA\`"
    echo "- Port commit: \`$PORT_COMMIT\`"
    echo "- Branch: \`$PORT_BRANCH\`"
    echo
    echo '### Diff summary'
    echo '\`\`\`'
    tail -n 10 "$REPORT_DIR/diffstat.txt"
    echo '\`\`\`'
  } >> "$GITHUB_STEP_SUMMARY"
fi
