#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Host tests for the RX-capture logic that has no Arduino dependency
# (src/helpers/rxcapture): payload builders, gates and the queue.
#
# Usage: test/rxcapture/run.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
work="$(mktemp -d)"
c++ -std=c++17 -O1 -Wall -Wextra -I"$root/src/helpers/rxcapture" \
    -o "$work/harness" "$here/harness.cpp" "$root"/src/helpers/rxcapture/*.cpp
"$work/harness"
