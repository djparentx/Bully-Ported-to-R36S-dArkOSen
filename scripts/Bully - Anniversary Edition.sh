#!/bin/bash
# Bully: Anniversary Edition for dArkOSen (RK3326, Mali-G31).
# The game folder is ./bully next to this script (ports/bully on /roms or /roms2).
# Override any BULLY_* or SDL_* variable from the environment.

GAMEDIR="$(cd "$(dirname "$0")/bully" && pwd)" || exit 1
cd "$GAMEDIR" || exit 1

LOG="$GAMEDIR/bully.log"
LOCK=/dev/shm/bully-lock
SAVED=/dev/shm/bully-clocks
CPU=/sys/devices/system/cpu/cpufreq/policy0/scaling_governor
GPU=""
for g in /sys/class/devfreq/*.gpu; do
	[ -d "$g" ] && GPU="$g" && break
done

# one start at a time; fd 9 is inherited by the game, so the lock lives as long as it runs
exec 9> "$LOCK" || { echo "$(date): cannot open $LOCK" >> "$LOG"; exit 1; }
if ! flock -n 9; then
	echo "$(date): Bully is running already; this start was refused" >> "$LOG"
	exit 1
fi
exec > "$LOG" 2>&1

kv() { printf '%-18s %s\n' "$1" "$2"; }
rd() { cat "$1" 2> /dev/null || echo none; }

# root writes for sysfs: sudo without a password, or nothing
SUDO=""
if [ "$(id -u)" -ne 0 ]; then
	if sudo -n true 2> /dev/null; then SUDO="sudo -n"; else SUDO="none"; fi
fi
put() {
	[ "$SUDO" = none ] && return 1
	echo "$1" | $SUDO tee "$2" > /dev/null 2>&1
}

# GPU: 600 MHz hangs the GPU under load on these boards (kernel hard-stop, ~8.7 s)
GPU_MAX_HZ="${BULLY_GPU_MAX_HZ:-520000000}"

lowest_step() { tr ' ' '\n' < "$1/available_frequencies" 2> /dev/null | grep . | sort -n | head -n 1; }

# highest available step at or below $2 (the lowest step if none is)
step_at_most() {
	local steps target
	steps=$(tr ' ' '\n' < "$1/available_frequencies" 2> /dev/null | grep . | sort -n)
	target=$(echo "$steps" | awk -v max="$2" '$1 <= max { t = $1 } END { print t }')
	[ -z "$target" ] && target=$(echo "$steps" | head -n 1)
	echo "$target"
}

# devfreq node, frequency: min/max/min so it lands whichever way it moves
pin() {
	[ -n "$1" ] && [ -n "$2" ] || return
	put "$2" "$1/min_freq"
	put "$2" "$1/max_freq"
	put "$2" "$1/min_freq"
}

# devfreq node, saved min, saved max
unpin() {
	[ -n "$1" ] || return
	local low
	low=$(lowest_step "$1")
	[ -n "$low" ] && put "$low" "$1/min_freq"
	[ -n "$3" ] && [ "$3" != none ] && put "$3" "$1/max_freq"
	[ -n "$2" ] && [ "$2" != none ] && put "$2" "$1/min_freq"
}

restore() {
	if [ -f "$SAVED" ]; then
		read -r s_gov s_gpu_min s_gpu_max < "$SAVED"
		[ -n "$s_gov" ] && [ "$s_gov" != none ] && put "$s_gov" "$CPU"
		unpin "$GPU" "$s_gpu_min" "$s_gpu_max"
		rm -f "$SAVED"
	fi
}
restore
echo "$(rd "$CPU") $(rd "$GPU/min_freq") $(rd "$GPU/max_freq")" > "$SAVED"
trap restore EXIT

put performance "$CPU"
[ -n "$GPU" ] && pin "$GPU" "$(step_at_most "$GPU" "$GPU_MAX_HZ")"

export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-kmsdrm}"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-alsa}"

# the handheld's controls from PortMaster; fallback is the GO-Super Gamepad mapping
for d in /opt/system/Tools/PortMaster /opt/tools/PortMaster /roms/tools/PortMaster /roms2/tools/PortMaster /roms/ports/PortMaster /roms2/ports/PortMaster; do
	if [ -f "$d/control.txt" ]; then
		controlfolder="$d"
		source "$d/control.txt"
		get_controls
		break
	fi
done
if [ -n "$sdl_controllerconfig" ]; then
	export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
elif [ -z "$SDL_GAMECONTROLLERCONFIG" ]; then
	_MAP="a:b1,b:b0,x:b2,y:b3,leftshoulder:b4,rightshoulder:b5,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,dpup:h0.1,leftx:a0,lefty:a1,rightx:a2,righty:a3,leftstick:b14,rightstick:b15,lefttrigger:b6,righttrigger:b7,back:b12,start:b13,"
	export SDL_GAMECONTROLLERCONFIG="190000004b4800000010000000010000,GO-Super Gamepad,$_MAP
1900bb3e4b4800000011000000010000,GO-Super Gamepad,$_MAP"
fi

echo "Bully for dArkOSen, $(date)"
kv "device"           "$(cat /home/ark/.config/.DEVICE 2> /dev/null)"
kv "kernel"           "$(uname -sr)"
kv "sdl2"             "$(readlink -f /usr/lib/aarch64-linux-gnu/libSDL2-2.0.so.0 2> /dev/null)"
kv "mem available"    "$(grep MemAvailable /proc/meminfo | tr -s ' ' | cut -d' ' -f2-)"
kv "fb0 modes"        "$(cat /sys/class/graphics/fb0/modes 2> /dev/null)"
kv "root helper"      "${SUDO:-root}"
kv "cpu governor"     "$(rd "$CPU")"
kv "gpu devfreq"      "${GPU:-none}"
kv "gpu min/max/cur"  "$(rd "$GPU/min_freq") / $(rd "$GPU/max_freq") / $(rd "$GPU/cur_freq")"
kv "dmc devfreq"      "${DMC:-none}"
kv "dmc min/max/cur"  "$(rd "$DMC/min_freq") / $(rd "$DMC/max_freq") / $(rd "$DMC/cur_freq")"
kv "SDL_VIDEODRIVER"  "$SDL_VIDEODRIVER"
kv "SDL_AUDIODRIVER"  "$SDL_AUDIODRIVER"
kv "controls"         "${controlfolder:-fallback mapping}"

# the game files come from the APK: the binary installs them on first start
if [ ! -s bully ]; then
	echo "missing in $GAMEDIR: bully"
	exit 1
fi
[ -x bully ] || chmod +x bully

sync
put 3 /proc/sys/vm/drop_caches

./bully &
child=$!
trap 'kill -TERM "$child" 2> /dev/null' TERM INT HUP
while :; do
	wait "$child"
	status=$?
	kill -0 "$child" 2> /dev/null || break
done
trap - TERM INT HUP
echo "exit status $status"
# 0: Select+Start or quit from the game; 130 and 143: stopped by a signal;
# 137: killed, most likely by the kernel when memory ran out
case "$status" in
0 | 130 | 143) ;;
137) echo "Bully was killed (137): most likely out of memory" ;;
*) echo "Bully stopped unexpectedly (exit status $status)" ;;
esac
exit "$status"