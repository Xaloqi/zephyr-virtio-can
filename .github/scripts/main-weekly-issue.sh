#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Tracking issue for a failing weekly run against Zephyr main: ONE, stable title.
#   main-weekly-issue.sh <pass|fail> <run-url> <dir with sha.txt, first-failure.txt, tail.txt>
# fail: open it, or comment on the open one. pass: comment with the SHA and close it.
# DRY_RUN=1: look up the issue (read-only) and print what would be done. TITLE_PREFIX is for tests.
set -euo pipefail
RESULT=$1; URL=$2; DIR=$3
TITLE="${TITLE_PREFIX:-}Breaks against Zephyr main"
DRY=${DRY_RUN:-0}
SHA=$(cat "$DIR/sha.txt" 2>/dev/null || echo unknown)
NUM=$(gh issue list --state open --limit 200 --json number,title \
      --jq "[.[] | select(.title == \"$TITLE\")] | sort_by(.number) | .[0].number // empty")
act() { if [ "$DRY" = 1 ]; then echo "DRY RUN would run: gh $*"; else gh "$@"; fi; }

if [ "$RESULT" = fail ]; then
  BODY=$(printf 'The weekly build and test against Zephyr `main` failed.\n\nZephyr: `%s`\nRun: %s\n\nFirst failure: `%s`\n\n<details><summary>Last lines of the log</summary>\n\n```\n%s\n```\n</details>\n\nReport-only: the v4.4.1 job is the release gate and is not affected. Logs and twister.json are attached to the run for 14 days.' \
        "$SHA" "$URL" "$(head -n1 "$DIR/first-failure.txt" 2>/dev/null || true)" "$(cat "$DIR/tail.txt" 2>/dev/null || echo '(no log captured)')")
  if [ -n "$NUM" ]; then echo "open tracking issue #$NUM exists: commenting"; act issue comment "$NUM" --body "$BODY"
  else echo "no open tracking issue titled '$TITLE': opening one"; act issue create --title "$TITLE" --body "$BODY"; fi
else
  if [ -n "$NUM" ]; then
    echo "run passes and issue #$NUM is open: commenting and closing"
    act issue comment "$NUM" --body "Passes again against Zephyr \`main\` at \`$SHA\`: $URL"
    act issue close "$NUM"
  else echo "run passes, no open tracking issue: nothing to do"; fi
fi
