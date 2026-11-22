#!/usr/bin/env bash
# Source-priority port: import gaokun3/kernel 7.2.15-bore, then restore
# explicitly selected local gaokun3 features on top of the coherent source.
set -Eeuo pipefail

DEST_BRANCH="gaokun3"
PORT_BRANCH="port/7.2.15-bore-source-first"
STABLE_TAG="v7.2.14"
UPSTREAM_SHA="206f40c6879ad336544e3dbb1131b34cd355097f"
REPORT_DIR="${REPORT_DIR:-port-report}"
mkdir -p "$REPORT_DIR"

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
say() { printf '\n==> %s\n' "$*" >&2; }

git rev-parse --is-inside-work-tree >/dev/null || die "Run in a git repository"
git remote get-url origin >/dev/null || die "Missing origin remote"

say "Fetch destination $DEST_BRANCH"
git fetch --no-tags --depth=1 origin \
  "refs/heads/$DEST_BRANCH:refs/remotes/origin/$DEST_BRANCH"
DEST_SHA="$(git rev-parse "refs/remotes/origin/$DEST_BRANCH^{commit}")"

if ! git remote get-url gaokun3-bore >/dev/null 2>&1; then
  git remote add gaokun3-bore https://github.com/gaokun3/kernel.git
fi
say "Fetch pinned upstream 7.2.15-bore: $UPSTREAM_SHA"
git fetch --no-tags --depth=1 gaokun3-bore "$UPSTREAM_SHA"
SOURCE_SHA="$(git rev-parse 'FETCH_HEAD^{commit}')"
[[ "$SOURCE_SHA" == "$UPSTREAM_SHA" ]] || die "Unexpected source commit"

if ! git remote get-url stable-rolling >/dev/null 2>&1; then
  git remote add stable-rolling https://github.com/gregkh/linux.git
fi
say "Fetch clean Linux stable base $STABLE_TAG"
git fetch --no-tags --depth=1 stable-rolling \
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

say "Three-way merge with upstream 7.2.15-bore as the preferred side"
if git merge-tree --write-tree --merge-base="$BASE_SHA" \
    "$SOURCE_SHA" "$DEST_SHA" > "$REPORT_DIR/merge-tree.log" 2>&1; then
  MERGED_TREE="$(head -n 1 "$REPORT_DIR/merge-tree.log")"
  say "Clean three-way merge"
else
  say "Overlapping changes found; favor upstream hunks and preserve non-conflicting local changes"
  grep '^CONFLICT ' "$REPORT_DIR/merge-tree.log" \
    | sed 's/^.*Merge conflict in //' \
    | sort -u > "$REPORT_DIR/conflicts.txt" || true
  [[ -s "$REPORT_DIR/conflicts.txt" ]] || {
    tail -n 100 "$REPORT_DIR/merge-tree.log" >&2
    die "Merge failed without identifiable conflicts"
  }
  if ! git merge-tree --write-tree --merge-base="$BASE_SHA" -X ours \
      "$SOURCE_SHA" "$DEST_SHA" > "$REPORT_DIR/resolved-merge.log" 2>&1; then
    tail -n 100 "$REPORT_DIR/resolved-merge.log" >&2
    die "Source-priority merge failed"
  fi
  MERGED_TREE="$(head -n 1 "$REPORT_DIR/resolved-merge.log")"
  say "Source-priority draft contains $(wc -l < "$REPORT_DIR/conflicts.txt") conflict paths"
fi
[[ "$MERGED_TREE" =~ ^[[:xdigit:]]{40,64}$ ]] || die "Invalid merge-tree result"

say "Reapply local gaokun3 touchscreen and EC wakeup configuration"
DTS_PATH="arch/arm64/boot/dts/qcom/sc8280xp-huawei-gaokun3.dts"
git show "$DEST_SHA:$DTS_PATH" > "$REPORT_DIR/dest-gaokun3.dts"
git show "$MERGED_TREE:$DTS_PATH" > "$REPORT_DIR/merged-gaokun3.dts"
python3 - "$REPORT_DIR/dest-gaokun3.dts" "$REPORT_DIR/merged-gaokun3.dts" "$REPORT_DIR/final-gaokun3.dts" <<'PY'
import pathlib, sys
dest_path, merged_path, out_path = map(pathlib.Path, sys.argv[1:])
dest = dest_path.read_text()
merged = merged_path.read_text()
start, end = "\n&i2c4 {", "\n&i2c15 {"
def section(s):
    a = s.find(start)
    b = s.find(end, a + len(start))
    if a < 0 or b < 0:
        raise SystemExit("could not locate local touchscreen block boundaries")
    return a, b
da, db = section(dest)
ma, mb = section(merged)
merged = merged[:ma] + dest[da:db] + merged[mb:]
# This wake source is safe only with the local EC suspend/IRQ quiescing patch.
old = "\t\t// not stable yet, so comment out it\n\t\t// wakeup-source;"
new = ("\t\t/* The EC IRQ is quiesced across suspend before enabling wake events. */\n"
       "\t\twakeup-source;")
if old in merged:
    merged = merged.replace(old, new, 1)
elif "wakeup-source;" not in merged:
    raise SystemExit("could not find EC wakeup property insertion point")
if "touchscreen_hid: touchscreen@4f" not in merged:
    raise SystemExit("local I2C-HID touchscreen was not restored")
if "touchscreen_secondary: touchscreen@0" not in merged:
    raise SystemExit("local secondary touchscreen was not restored")
out_path.write_text(merged)
PY

say "Reapply requested tracing and BORE config options to gaokun3_defconfig"
DEFCONFIG_PATH="arch/arm64/configs/gaokun3_defconfig"
git show "$MERGED_TREE:$DEFCONFIG_PATH" > "$REPORT_DIR/merged-gaokun3_defconfig"
python3 - "$REPORT_DIR/merged-gaokun3_defconfig" "$REPORT_DIR/final-gaokun3_defconfig" <<'PY'
import pathlib, sys
src, dst = map(pathlib.Path, sys.argv[1:])
lines = src.read_text().splitlines()
required = ["SCHED_BORE", "KPROBES", "FTRACE", "KPROBE_EVENTS", "TRACING", "TRACEFS_FS"]
for name in required:
    lines = [line for line in lines
             if line != f"# CONFIG_{name} is not set"
             and not line.startswith(f"CONFIG_{name}=")]
    lines.append(f"CONFIG_{name}=y")
dst.write_text("\n".join(lines) + "\n")
PY

say "Assemble final tree and porting notes"
PORT_INDEX="${RUNNER_TEMP:-/tmp}/gaokun3-port-index-$$"
GIT_INDEX_FILE="$PORT_INDEX" git read-tree "$MERGED_TREE"
for spec in \
  "$REPORT_DIR/final-gaokun3.dts:$DTS_PATH" \
  "$REPORT_DIR/final-gaokun3_defconfig:$DEFCONFIG_PATH"; do
  file="${spec%%:*}"
  path="${spec#*:}"
  blob="$(git hash-object -w "$file")"
  GIT_INDEX_FILE="$PORT_INDEX" git update-index --add --cacheinfo "100644,$blob,$path"
done

cat > "$REPORT_DIR/PORTING_NOTES.md" <<EOF
# Linux 7.2.15-bore source-priority port

Pinned source: https://github.com/gaokun3/kernel/tree/7.2.15-bore
Source commit: `$SOURCE_SHA`
Destination base: `$DEST_SHA`
Synthetic common base: Linux `$STABLE_TAG`.

## Merge policy

This candidate prefers the complete upstream 7.2.15-bore implementation for
overlapping hunks (`git merge-tree -X ours`, with upstream as the first side).
Non-overlapping local changes are retained. This avoids the mixed-version
UCSI and battery implementations produced by the earlier local-priority draft.

The following local changes were explicitly restored:
- gaokun3 I2C-HID touchscreen and dual-Himax touchscreen description
- EC wakeup-source property (paired with the local EC suspend/IRQ quiescing code)
- CONFIG_SCHED_BORE and the requested KPROBES/FTRACE/KPROBE_EVENTS/TRACING/TRACEFS_FS options

## Manual review still required

The initial strict merge reported these overlapping paths:
EOF
if [[ -s "$REPORT_DIR/conflicts.txt" ]]; then
  while IFS= read -r path; do printf -- '- `%s`\n' "$path"; done < "$REPORT_DIR/conflicts.txt" >> "$REPORT_DIR/PORTING_NOTES.md"
fi
cat >> "$REPORT_DIR/PORTING_NOTES.md" <<'EOF'

This is a build candidate, not a validated device release. Review the conflict
paths, complete the kernel/DTB build, and test suspend, touchscreen, cameras,
Wi-Fi, USB-C/DP altmode, battery, and FastRPC on hardware before merging.

Assisted by ChatGPT.
EOF

NOTE_BLOB="$(git hash-object -w "$REPORT_DIR/PORTING_NOTES.md")"
GIT_INDEX_FILE="$PORT_INDEX" git update-index --add --cacheinfo "100644,$NOTE_BLOB,PORTING_NOTES.md"
MERGED_TREE="$(GIT_INDEX_FILE="$PORT_INDEX" git write-tree)"
rm -f "$PORT_INDEX"

say "Validate imported source features and restored local features"
git show "$MERGED_TREE:Makefile" | grep -qx 'SUBLEVEL = 15' || die "Not Linux 7.2.15"
grep -q '^config SCHED_BORE$' <(git show "$MERGED_TREE:init/Kconfig") || die "BORE Kconfig missing"
grep -q 'CONFIG_SCHED_BORE' <(git show "$MERGED_TREE:kernel/sched/fair.c") || die "BORE integration missing"
grep -q 'struct work_struct init_sync_work;' <(git show "$MERGED_TREE:drivers/usb/typec/ucsi/ucsi_huawei_gaokun.c") || die "Upstream UCSI structure not imported"
grep -q 'static int gaokun_psy_capacity_level' <(git show "$MERGED_TREE:drivers/power/supply/huawei-gaokun-battery.c") || die "Battery capacity-level helper missing"
for option in SCHED_BORE KPROBES FTRACE KPROBE_EVENTS TRACING TRACEFS_FS; do
  grep -qx "CONFIG_${option}=y" <(git show "$MERGED_TREE:$DEFCONFIG_PATH") || die "CONFIG_${option} not enabled"
done
grep -qx 'CONFIG_KVM=y' <(git show "$MERGED_TREE:$DEFCONFIG_PATH") || die "KVM disabled"
grep -qx 'CONFIG_QCOM_FASTRPC=m' <(git show "$MERGED_TREE:$DEFCONFIG_PATH") || die "FastRPC module disabled"
grep -qx 'CONFIG_VIDEO_OV13B10=m' <(git show "$MERGED_TREE:$DEFCONFIG_PATH") || die "OV13B10 camera module missing"
grep -q 'touchscreen_hid: touchscreen@4f' <(git show "$MERGED_TREE:$DTS_PATH") || die "I2C-HID touchscreen missing"
grep -q 'touchscreen_secondary: touchscreen@0' <(git show "$MERGED_TREE:$DTS_PATH") || die "Secondary touchscreen missing"
git cat-file -e "$MERGED_TREE:arch/arm64/boot/dts/qcom/sc8280xp-huawei-gaokun3-camera.dtsi" || die "Camera DTSI missing"

git diff --stat "$DEST_SHA" "$MERGED_TREE" > "$REPORT_DIR/diffstat.txt"
git diff --name-status "$DEST_SHA" "$MERGED_TREE" > "$REPORT_DIR/changed-files.txt"
printf 'merged_tree=%s\n' "$MERGED_TREE" >> "$REPORT_DIR/refs.txt"

PORT_COMMIT="$(
  GIT_AUTHOR_NAME="GitHub Actions" \
  GIT_AUTHOR_EMAIL="41898282+github-actions[bot]@users.noreply.github.com" \
  GIT_COMMITTER_NAME="GitHub Actions" \
  GIT_COMMITTER_EMAIL="41898282+github-actions[bot]@users.noreply.github.com" \
  git -c commit.gpgsign=false commit-tree "$MERGED_TREE" -p "$DEST_SHA" \
    -m "Linux 7.2.15-bore: source-priority port onto gaokun3" \
    -m "Merge base: Linux $STABLE_TAG (gregkh/linux)" \
    -m "Source: https://github.com/gaokun3/kernel/tree/7.2.15-bore" \
    -m "Upstream-Commit: $SOURCE_SHA" \
    -m "Assisted by ChatGPT"
)"
printf 'port_commit=%s\n' "$PORT_COMMIT" >> "$REPORT_DIR/refs.txt"
git update-ref "refs/heads/$PORT_BRANCH" "$PORT_COMMIT"

say "Port generated: $PORT_COMMIT"
cat "$REPORT_DIR/refs.txt"
head -n 80 "$REPORT_DIR/diffstat.txt" || true
say "Push review branch $PORT_BRANCH (never force)"
git push origin "$PORT_COMMIT:refs/heads/$PORT_BRANCH"

if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
  {
    echo '## gaokun3 Linux 7.2.15-bore source-priority port'
    echo
    echo "- Base: `$DEST_SHA`"
    echo "- Source: `$SOURCE_SHA`"
    echo "- Port commit: `$PORT_COMMIT`"
    echo "- Branch: `$PORT_BRANCH`"
    echo
    echo '### Diff summary'
    echo '```'
    tail -n 10 "$REPORT_DIR/diffstat.txt"
    echo '```'
  } >> "$GITHUB_STEP_SUMMARY"
fi
