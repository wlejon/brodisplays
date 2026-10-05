#!/usr/bin/env bash
# Runs a command as a client of a private headless sway (the ambient Wayland
# display of a Linux CI job): its own XDG_RUNTIME_DIR, the pixman renderer,
# no input devices, no X11. Exits with the command's status.
#
#   with-headless-sway.sh ctest --test-dir build -R test_linux_query
set -uo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
rt="$(mktemp -d)"
chmod 700 "$rt"

unset DISPLAY WAYLAND_DISPLAY
export XDG_RUNTIME_DIR="$rt"
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
    sway -c "$here/sway.conf" >"$rt/sway.log" 2>&1 &
sway_pid=$!

socket=""
for _ in $(seq 150); do
    for s in "$rt"/wayland-*; do
        case "$s" in *.lock | *"*") continue ;; esac
        socket="$(basename "$s")"
    done
    [ -n "$socket" ] && break
    kill -0 "$sway_pid" 2>/dev/null || break
    sleep 0.1
done

if [ -z "$socket" ]; then
    echo "headless sway did not come up" >&2
    cat "$rt/sway.log" >&2
    exit 1
fi

export WAYLAND_DISPLAY="$socket"
echo "headless sway on $WAYLAND_DISPLAY"
"$@"
rc=$?

kill "$sway_pid" 2>/dev/null
wait "$sway_pid" 2>/dev/null
rm -rf "$rt"
exit "$rc"
