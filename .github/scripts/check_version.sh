#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# README status line, newest CHANGELOG heading and latest git tag must agree
# (#6: the README said 0.1.0 after v0.2.0 shipped). The README may be ahead of
# the tag while a release is being prepared, never behind it.
set -euo pipefail
cd "$(dirname "$0")/../.."

readme=$(sed -nE 's/^\*\*Status: [^,]*, ([0-9]+\.[0-9]+\.[0-9]+)\.\*\*.*/\1/p' README.md | head -1)
changelog=$(sed -nE 's/^## ([0-9]+\.[0-9]+\.[0-9]+) .*/\1/p' CHANGELOG.md | head -1)
tag=$(git tag --list 'v[0-9]*.[0-9]*.[0-9]*' | sed 's/^v//' | sort -V | tail -1)

[ -n "$readme" ]    || { echo "::error::no 'Status: ..., X.Y.Z.' line in README.md"; exit 1; }
[ -n "$changelog" ] || { echo "::error::no '## X.Y.Z' heading in CHANGELOG.md"; exit 1; }
echo "README=$readme CHANGELOG=$changelog latest-tag=${tag:-none}"

[ "$readme" = "$changelog" ] || {
  echo "::error::README status says $readme, newest CHANGELOG entry is $changelog"; exit 1; }
if [ -n "$tag" ] && [ "$(printf '%s\n%s\n' "$tag" "$readme" | sort -V | tail -1)" != "$readme" ]; then
  echo "::error::README says $readme, behind latest tag v$tag"; exit 1
fi
