#!/usr/bin/env bash
# Runs ctest with the given arguments, then prints why each skipped test
# skipped (its own "SKIPPED: <reason>" line) and the notes passing tests leave,
# so a job's log says what it did not cover and why. Exits with ctest's status.
#
#   ctest.sh --test-dir build -C Release
set -uo pipefail

ctest --output-on-failure "$@"
rc=$?

dir=build
prev=""
for a in "$@"; do
    [ "$prev" = "--test-dir" ] && dir="$a"
    prev="$a"
done
# Whole tests that skipped, and the notes passing tests leave about what they
# could not check here (no oracle, a refused alternate), each under its test.
# A skip reason may run over several lines (a server's log tail); it ends with
# the test's output.
reasons="$(awk '/^[0-9]+\/[0-9]+ Testing: / { t = $3 }
               /SKIPPED/ { p = 1 }
               /<end of output>/ { p = 0 }
               p || /Note: |refused: / { print t ": " $0 }' \
    "$dir"/Testing/Temporary/LastTest*.log 2>/dev/null)"
if [ -n "$reasons" ]; then
    echo
    echo "Skipped, and notes on what was not checked:"
    echo "$reasons"
fi
exit "$rc"
