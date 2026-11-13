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
# gnu++11: the firmware builds with it, so the tests must not accept more.
c++ -std=gnu++11 -O1 -Wall -Wextra -I"$root/src/helpers/rxcapture" \
    -o "$work/harness" "$here/harness.cpp" "$root"/src/helpers/rxcapture/*.cpp
"$work/harness"
