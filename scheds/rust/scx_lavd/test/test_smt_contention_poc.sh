#!/usr/bin/env bash

set -euo pipefail

TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP_DIR="$(mktemp -d /tmp/scx_smt_contention.XXXXXX)"
WORKER_BIN="$TMP_DIR/smt_contention_worker"
CPU_PAIR="${CPU_PAIR:-}"
RUNS="${RUNS:-5}"
DURATION_MS="${DURATION_MS:-2000}"
NOISE_SPINUP_MS="${NOISE_SPINUP_MS:-150}"
MIN_SLOWDOWN_PCT="${MIN_SLOWDOWN_PCT:-3}"
BASELINE_LOG="$TMP_DIR/baseline.log"
CONTENDED_LOG="$TMP_DIR/contended.log"

cleanup() {
	pkill -P $$ 2>/dev/null || true
	wait 2>/dev/null || true
	if [[ -z "${KEEP_TMP:-}" ]]; then
		rm -rf "$TMP_DIR"
	else
		echo "Keeping tmp dir: $TMP_DIR" >&2
	fi
}

fail() {
	echo "FAIL: $*" >&2
	exit 1
}

find_sibling_pair() {
	local topo list cpu0 cpu1

	for topo in /sys/devices/system/cpu/cpu[0-9]*/topology/thread_siblings_list; do
		list="$(<"$topo")"
		if [[ "$list" == *,* || "$list" == *-* ]]; then
			cpu0="$(awk -F'[-,]' '{print $1}' <<<"$list")"
			cpu1="$(awk -F'[-,]' '{print $2}' <<<"$list")"
			if [[ "$cpu0" != "$cpu1" ]]; then
				echo "$cpu0,$cpu1"
				return 0
			fi
		fi
	done

	return 1
}

extract_field() {
	local key="$1"
	awk -v key="$key" '{
		for (i = 1; i <= NF; i++) {
			if ($i ~ ("^" key "=")) {
				split($i, a, "=");
				print a[2];
				exit;
			}
		}
	}'
}

median() {
	printf '%s\n' "$@" | sort -n | awk '
		{ vals[NR] = $1 }
		END {
			if (NR == 0) exit 1;
			if (NR % 2 == 1) {
				print vals[(NR + 1) / 2];
			} else {
				print int((vals[NR / 2] + vals[(NR / 2) + 1]) / 2);
			}
		}'
}

trap cleanup EXIT INT TERM

if [[ -n "$CPU_PAIR" ]]; then
	PAIR="$CPU_PAIR"
else
	PAIR="$(find_sibling_pair)" || fail "failed to find an SMT sibling pair; set CPU_PAIR=CPU0,CPU1"
fi

CPU0="${PAIR%,*}"
CPU1="${PAIR#*,}"

cc -O2 -Wall -Wextra \
	"$TEST_DIR/smt_contention_worker.c" \
	-o "$WORKER_BIN"

: > "$BASELINE_LOG"
: > "$CONTENDED_LOG"

echo "Using SMT sibling pair: $CPU0,$CPU1"
echo "Runs: $RUNS, duration: ${DURATION_MS}ms, threshold: ${MIN_SLOWDOWN_PCT}%"

baseline_ops=()
contended_ops=()

for run in $(seq 1 "$RUNS"); do
	out="$("$WORKER_BIN" --cpu "$CPU0" --duration-ms "$DURATION_MS" --label "baseline-$run")"
	echo "$out" | tee -a "$BASELINE_LOG"
	ops="$(extract_field ops <<<"$out")"
	[[ -n "$ops" ]] || fail "failed to parse baseline ops on run $run"
	baseline_ops+=("$ops")

	"$WORKER_BIN" --cpu "$CPU1" --duration-ms "$((DURATION_MS + 1000))" --label "noise-$run" >/dev/null &
	noise_pid=$!
	sleep "$(awk "BEGIN { printf \"%.3f\", $NOISE_SPINUP_MS / 1000 }")"

	out="$("$WORKER_BIN" --cpu "$CPU0" --duration-ms "$DURATION_MS" --label "contended-$run")"
	echo "$out" | tee -a "$CONTENDED_LOG"
	ops="$(extract_field ops <<<"$out")"
	[[ -n "$ops" ]] || fail "failed to parse contended ops on run $run"
	contended_ops+=("$ops")

	kill "$noise_pid" 2>/dev/null || true
	wait "$noise_pid" 2>/dev/null || true
done

baseline_median="$(median "${baseline_ops[@]}")"
contended_median="$(median "${contended_ops[@]}")"

slowdown_pct="$(awk -v base="$baseline_median" -v cont="$contended_median" '
	BEGIN {
		if (base == 0) {
			print "0.00";
		} else {
			printf "%.2f", ((base - cont) * 100.0) / base;
		}
	}')"

echo "baseline median ops:  $baseline_median"
echo "contended median ops: $contended_median"
echo "slowdown percent:     $slowdown_pct"

awk -v slowdown="$slowdown_pct" -v min="$MIN_SLOWDOWN_PCT" '
	BEGIN {
		if (slowdown + 0 < min + 0)
			exit 1;
	}' || fail "SMT sibling contention was weaker than expected (slowdown=${slowdown_pct}% threshold=${MIN_SLOWDOWN_PCT}%)"

echo "PASS: single-thread throughput drops when the SMT sibling is saturated"
