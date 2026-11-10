#!/usr/bin/env bash
# Verify the release artifacts with readers that did not write them (port of MoofTCP's).
#   release-verify.sh [--self-test] <version>      e.g. release-verify.sh 1.0
# Reads dist/MoofPort-<version>.sit.hqx  (macutils hexbin, and unar),
#       dist/MoofPort-<version>.sit      (unar),
#       dist/MoofPort-<version>.dsk      (hfsutils hmount/hls/hcopy -m),
#       dist/MoofPort-<version>.sha256   (recomputed)
# and compares names, type/creator, Finder flags, fork lengths and sha256 of both forks with
# build-mac/{MoofPortInstall,MoofStat,MoofTune}.bin and the Read Me derived from
# docs/release/ReadMe.txt; also re-runs scripts/relvers.py's installer checks. Details:
# scripts/relverify.py, docs/dev/RELEASE.md. Exit 0 only if every check passes.
# --self-test: the real artifacts must pass, and broken copies of each format must FAIL.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec python3 -I "$ROOT/scripts/relverify.py" "$@"
