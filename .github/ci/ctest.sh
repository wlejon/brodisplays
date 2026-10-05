#!/usr/bin/env bash
# Runs ctest with the given arguments, then prints why each skipped test
# skipped (its own "SKIPPED: <reason>" line), so a job's log says what it did
# not cover and why. Exits with ctest's status.
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
reasons="$(grep -h "SKIP" "$dir"/Testing/Temporary/LastTest*.log 2>/dev/null | sort -u)"
if [ -n "$reasons" ]; then
    echo
    echo "Skip reasons:"
    echo "$reasons"
fi
exit "$rc"
