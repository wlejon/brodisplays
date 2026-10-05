#!/usr/bin/env bash
# Starts an Xorg server on the dummy video driver (xorg-dummy.conf beside this
# script) as the ambient X display of a Linux CI job, as the runner user, and
# waits until it answers RandR. It outlives the step and dies with the job.
#
#   start-xorg-dummy.sh :99
set -euo pipefail

display="${1:-:99}"
here="$(cd "$(dirname "$0")" && pwd)"

# The server itself, not the setuid wrapper (which would refuse an absolute
# -config path from an unprivileged user).
server=/usr/lib/xorg/Xorg
[ -x "$server" ] || server=/usr/lib/Xorg

state="${RUNNER_TEMP:-/tmp}/xorg${display#:}"
mkdir -p "$state/conf.d"
setsid nohup "$server" "$display" -config "$here/xorg-dummy.conf" -configdir "$state/conf.d" \
    -logfile "$state/xorg.log" -nolisten tcp -noreset >"$state/xorg.out" 2>&1 </dev/null &

for _ in $(seq 150); do
    if xrandr -display "$display" >/dev/null 2>&1; then
        xrandr -display "$display"
        exit 0
    fi
    sleep 0.1
done

echo "the Xorg dummy server on $display did not come up" >&2
cat "$state/xorg.out" "$state/xorg.log" >&2 || true
exit 1
