#!/usr/bin/env bash
# TL;DR: End-to-end local SMT test that builds the helper worker, runs LAVD,
# injects task-local hints, and asserts the exclusive task blocks its sibling.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")"/../../../.. && pwd)"
SCX_LAVD_BIN="${SCX_LAVD_BIN:-$ROOT_DIR/target/release/scx_lavd}"
TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP_DIR="${TMP_DIR:-$(mktemp -d /tmp/scx_lavd_smt_test.XXXXXX)}"
WORKER_BIN="$TMP_DIR/scx_lavd_smt_worker"
MAP_PATH="${MAP_PATH:-/run/wds/shared_bpf/scx_lavd_test_map}"
CPU_PAIR="${CPU_PAIR:-}"
SMT_HINT_MODE="${SMT_HINT_MODE:-explicit}"
SMT_LAT_CRI_THRESHOLD="${SMT_LAT_CRI_THRESHOLD:-16000}"
SCHED_LOG="$TMP_DIR/scx_lavd.log"
TRACE_LOG="$TMP_DIR/trace.log"
PEER_LOG="$TMP_DIR/peer.log"
EXCL_LOG="$TMP_DIR/exclusive.log"
CPU0=""
CPU1=""
LATENCY_RUNS="${LATENCY_RUNS:-3}"
LATENCY_PHASE1_LOOPS="${LATENCY_PHASE1_LOOPS:-1000}"
LATENCY_PEER_MS="${LATENCY_PEER_MS:-15000}"
PEER_BATCH_ITERS="${PEER_BATCH_ITERS:-100000}"
LATENCY_PEER_BATCH_ITERS="${LATENCY_PEER_BATCH_ITERS:-200000}"
LATENCY_PEER_SLEEP_US="${LATENCY_PEER_SLEEP_US:-200}"
MIN_SPEEDUP_PCT="${MIN_SPEEDUP_PCT:-8}"
LATENCY_SLICE_MS="${LATENCY_SLICE_MS:-0}"
LATENCY_EXCLUSIVE_LAT_CRI="${LATENCY_EXCLUSIVE_LAT_CRI:-16000}"
LATENCY_SHARED_LAT_CRI="${LATENCY_SHARED_LAT_CRI:-128}"
TRACE_PIPE=""
TRACE_PID=""
SCHED_PID=""
PEER_PID=""
EXCL_PID=""
PEER_RUN_PID=""
EXCL_RUN_PID=""
SCHED_ARGS=()
ALLOW_VERIFIER_FALLBACK="${ALLOW_VERIFIER_FALLBACK:-1}"
RUN_LATENCY_IN_VERIFIER_FALLBACK="${RUN_LATENCY_IN_VERIFIER_FALLBACK:-0}"
VERIFIER_FALLBACK_MODE=0
REQUIRED_STARTUP_FLAGS=(--performance --log-level trace --task-hint-map "$MAP_PATH" --smt on)
PHASE1_SMT=1
PHASE2_SMT=0
PHASE1_LAT_CRI=60000
PHASE2_LAT_CRI=60000
TRIAL_EXCLUSIVE_SMT=1
TRIAL_EXCLUSIVE_LAT_CRI=60000
TRIAL_CONTROL_SMT=0
TRIAL_CONTROL_LAT_CRI=60000
EXPECT_PHASE1_RAW_SMT=1
EXPECT_PHASE2_RAW_SMT=0
EXPECT_FALLBACK_LOG=0

if [[ -n "${SCX_LAVD_EXTRA_ARGS:-}" ]]; then
  # shellcheck disable=SC2206
  extra_args=( ${SCX_LAVD_EXTRA_ARGS} )
  SCHED_ARGS+=("${extra_args[@]}")
fi

cleanup() {
  local pids=("$EXCL_PID" "$PEER_PID" "$TRACE_PID" "$SCHED_PID")

  for pid in "${pids[@]}"; do
    if [[ -n "$pid" ]]; then
      kill "$pid" 2>/dev/null || true
    fi
  done
  wait 2>/dev/null || true
  rm -f "$MAP_PATH"
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

proc_ticks() {
  local pid="$1"
  [[ -r "/proc/$pid/stat" ]] || fail "process $pid exited before sampling; inspect logs${KEEP_TMP:+ in $TMP_DIR}"
  awk '{print $14 + $15}' "/proc/$pid/stat"
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
      if (NR % 2 == 1)
        print vals[(NR + 1) / 2];
      else
        print int((vals[NR / 2] + vals[(NR / 2) + 1]) / 2);
    }'
}

find_trace_pipe() {
  for candidate in /sys/kernel/tracing/trace_pipe /sys/kernel/debug/tracing/trace_pipe; do
    if [[ -r "$candidate" ]]; then
      TRACE_PIPE="$candidate"
      return 0
    fi
  done
  return 1
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

dump_startup_debug() {
  local sched_log="$1"
  local state pid

  state="$(< /sys/kernel/sched_ext/state 2>/dev/null || echo missing)"
  pid="$(pgrep -xo scx_lavd 2>/dev/null || true)"

  echo "startup-debug: sched_ext_state=$state" >&2
  echo "startup-debug: map_path=$MAP_PATH exists=$( [[ -e "$MAP_PATH" ]] && echo yes || echo no )" >&2
  echo "startup-debug: pid=${pid:-missing}" >&2
  echo "startup-debug: cmdline=$( [[ -n "$pid" && -r "/proc/$pid/cmdline" ]] && tr '\0' ' ' < "/proc/$pid/cmdline" || echo missing )" >&2
  echo "startup-debug: last scheduler log lines" >&2
  tail -n 80 "$sched_log" >&2 || true
}

classify_startup_failure() {
  local sched_log="$1"

  if grep -q "BPF program is too large" "$sched_log" 2>/dev/null; then
    echo "BPF verifier instruction-limit cliff on this host/prod-equivalent args"
    return
  fi
  if grep -q "pointer arithmetic on mem_or_null prohibited" "$sched_log" 2>/dev/null; then
    echo "BPF verifier rejected nullable pointer arithmetic"
    return
  fi
  if grep -q "failed to load BPF program" "$sched_log" 2>/dev/null; then
    echo "BPF program load failed"
    return
  fi
  if grep -q "Permission denied (os error 13)" "$sched_log" 2>/dev/null; then
    echo "kernel verifier returned -EACCES"
    return
  fi
  echo "unknown startup failure"
}

ensure_prod_equiv_args() {
  local forbidden

  for forbidden in --simple-enqueue --partial --no-wake-sync --no-execve-migration-hooks; do
    if [[ " ${SCX_LAVD_EXTRA_ARGS:-} " == *" ${forbidden} "* ]]; then
      fail "SCX_LAVD_EXTRA_ARGS must not include ${forbidden}; local e2e must use prod-equivalent verifier path"
    fi
  done
}

assert_worker_log() {
  local log_path="$1"
  local label="$2"
  local expected_cpu="$3"

  grep -q "^${label} pid=.* phase=1 " "$log_path" \
    || fail "missing ${label} phase=1 record in $log_path"
  grep -q "^${label} pid=.* phase=1_done " "$log_path" \
    || fail "missing ${label} phase=1_done record in $log_path"
  grep -Eq "^${label} pid=.* cpu=${expected_cpu}\$" "$log_path" \
    || fail "${label} did not report expected cpu ${expected_cpu} in $log_path"
}

assert_worker_started() {
  local log_path="$1"
  local label="$2"
  local expected_cpu="$3"

  grep -q "^${label} pid=.* phase=1 " "$log_path" \
    || fail "missing ${label} phase=1 record in $log_path"
  grep -Eq "^${label} pid=.* cpu=${expected_cpu}\$" "$log_path" \
    || fail "${label} did not report expected cpu ${expected_cpu} in $log_path"
}

configure_hint_mode() {
  case "$SMT_HINT_MODE" in
    explicit)
      PHASE1_SMT=1
      PHASE2_SMT=0
      PHASE1_LAT_CRI=60000
      PHASE2_LAT_CRI=60000
      TRIAL_EXCLUSIVE_SMT=1
      TRIAL_EXCLUSIVE_LAT_CRI=60000
      TRIAL_CONTROL_SMT=0
      TRIAL_CONTROL_LAT_CRI=60000
      EXPECT_PHASE1_RAW_SMT=1
      EXPECT_PHASE2_RAW_SMT=0
      EXPECT_FALLBACK_LOG=0
      ;;
    lat-cri)
      PHASE1_SMT=0
      PHASE2_SMT=0
      PHASE1_LAT_CRI="$SMT_LAT_CRI_THRESHOLD"
      PHASE2_LAT_CRI=128
      TRIAL_EXCLUSIVE_SMT=0
      TRIAL_EXCLUSIVE_LAT_CRI="$SMT_LAT_CRI_THRESHOLD"
      TRIAL_CONTROL_SMT=0
      TRIAL_CONTROL_LAT_CRI=128
      EXPECT_PHASE1_RAW_SMT=0
      EXPECT_PHASE2_RAW_SMT=0
      EXPECT_FALLBACK_LOG=1
      ;;
    *)
      fail "unsupported SMT_HINT_MODE=$SMT_HINT_MODE (expected explicit or lat-cri)"
      ;;
  esac
}

start_scheduler_once() {
  local sched_log="$1"
  local trace_log="$2"
  shift 2
  local state=""

  : > "$sched_log"
  : > "$trace_log"
  rm -f "$MAP_PATH"
  : > "$(dirname "$TRACE_PIPE")/trace"
  cat "$TRACE_PIPE" > "$trace_log" &
  TRACE_PID=$!

  "$SCX_LAVD_BIN" \
    "${REQUIRED_STARTUP_FLAGS[@]}" \
    "${SCHED_ARGS[@]}" \
    "$@" \
    >"$sched_log" 2>&1 &
  SCHED_PID=$!

  for _ in $(seq 1 60); do
    state="$(< /sys/kernel/sched_ext/state 2>/dev/null || true)"
    if [[ -e "$MAP_PATH" ]] && [[ "$state" == "enabled" ]]; then
      return 0
    fi
    if ! kill -0 "$SCHED_PID" 2>/dev/null; then
      return 1
    fi
    sleep 0.25
  done

  if kill -0 "$SCHED_PID" 2>/dev/null &&
     [[ -e "$MAP_PATH" ]] &&
     grep -q "scheduler starts running" "$sched_log" 2>/dev/null; then
    return 0
  fi

  return 1
}

start_scheduler() {
  local sched_log="$1"
  local trace_log="$2"
  local failure=""

  VERIFIER_FALLBACK_MODE=0
  if start_scheduler_once "$sched_log" "$trace_log"; then
    return 0
  fi

  failure="$(classify_startup_failure "$sched_log")"
  if [[ "$ALLOW_VERIFIER_FALLBACK" == "1" && "$failure" == *"instruction-limit cliff"* ]]; then
    echo "startup-debug: prod-equivalent startup hit verifier cliff; retrying with --simple-enqueue" >&2
    stop_scheduler
    VERIFIER_FALLBACK_MODE=1
    if start_scheduler_once "$sched_log" "$trace_log" --simple-enqueue; then
      return 0
    fi
    failure="$(classify_startup_failure "$sched_log")"
  fi

  dump_startup_debug "$sched_log"
  if [[ ! -e "$MAP_PATH" ]]; then
    fail "scheduler did not pin task hint map (${failure})"
  fi
  fail "sched_ext did not enable (${failure})"
}

stop_scheduler() {
  if [[ -n "$PEER_PID" ]]; then
    kill "$PEER_PID" 2>/dev/null || true
    wait "$PEER_PID" 2>/dev/null || true
    PEER_PID=""
  fi
  if [[ -n "$EXCL_PID" ]]; then
    kill "$EXCL_PID" 2>/dev/null || true
    wait "$EXCL_PID" 2>/dev/null || true
    EXCL_PID=""
  fi
  if [[ -n "$SCHED_PID" ]]; then
    kill "$SCHED_PID" 2>/dev/null || true
    wait "$SCHED_PID" 2>/dev/null || true
    SCHED_PID=""
  fi
  if [[ -n "$TRACE_PID" ]]; then
    kill "$TRACE_PID" 2>/dev/null || true
    wait "$TRACE_PID" 2>/dev/null || true
    TRACE_PID=""
  fi
}

run_latency_trial() {
  local expect_exclusive="$1"
  local smt_flag="$2"
  local lat_cri="$3"
  local run_id="$4"
  local tag="lat_${expect_exclusive}_${run_id}"
  local sched_log="$TMP_DIR/${tag}.sched.log"
  local trace_log="$TMP_DIR/${tag}.trace.log"
  local peer_log="$TMP_DIR/${tag}.peer.log"
  local excl_log="$TMP_DIR/${tag}.exclusive.log"
  local elapsed_ms

  start_scheduler "$sched_log" "$trace_log"

  "$WORKER_BIN" \
    --label "peer-$tag" \
    --allowed-cpus "$CPU1" \
    --phase1-ms "$LATENCY_PEER_MS" \
    --batch-iters "$LATENCY_PEER_BATCH_ITERS" \
    --sleep-us-each-batch "$LATENCY_PEER_SLEEP_US" \
    --yield-each-batch \
    --sched-ext \
    >"$peer_log" 2>&1 &
  PEER_PID=$!

  sleep 0.3

  "$WORKER_BIN" \
    --label "exclusive-$tag" \
    --allowed-cpus "$CPU0" \
    --map-path "$MAP_PATH" \
    --slice-ms "$LATENCY_SLICE_MS" \
    --phase1-lat-cri "$lat_cri" \
    --phase1-loops "$LATENCY_PHASE1_LOOPS" \
    --phase1-smt "$smt_flag" \
    --sched-ext \
    >"$excl_log" 2>&1 &
  EXCL_PID=$!

  wait "$EXCL_PID"
  EXCL_PID=""
  elapsed_ms="$(grep 'phase=1_done' "$excl_log" | extract_field elapsed_ms)"
  [[ -n "$elapsed_ms" ]] || fail "failed to parse latency result from $excl_log"

  if [[ "$expect_exclusive" == "1" ]]; then
    grep -q "tld_smt_block:" "$trace_log" \
      || fail "missing latency-trial tld_smt_block printk"
  fi
  [[ "$(< /sys/kernel/sched_ext/state)" == "enabled" ]] \
    || fail "sched_ext disabled before latency trial completed"
  assert_worker_started "$peer_log" "peer-$tag" "$CPU1"
  assert_worker_log "$excl_log" "exclusive-$tag" "$CPU0"

  stop_scheduler
  echo "$elapsed_ms"
}

if [[ $EUID -ne 0 ]]; then
  fail "must run as root"
fi

if [[ ! -x "$SCX_LAVD_BIN" ]]; then
  fail "missing scx_lavd binary at $SCX_LAVD_BIN"
fi

ensure_prod_equiv_args
find_trace_pipe || fail "trace_pipe is required for BPF printk capture"

if [[ -n "$CPU_PAIR" ]]; then
  PAIR="$CPU_PAIR"
else
  PAIR="$(find_sibling_pair)" || fail "failed to find an SMT sibling pair; set CPU_PAIR=CPU0,CPU1 on an SMT-capable host"
fi
echo "Using SMT sibling pair: $PAIR"
IFS=, read -r CPU0 CPU1 <<< "$PAIR"
[[ -n "$CPU0" && -n "$CPU1" ]] || fail "failed to parse CPU pair '$PAIR'"

configure_hint_mode
echo "Using SMT hint mode: $SMT_HINT_MODE"

cc -O2 -Wall -Wextra -I"$TEST_DIR" \
  "$TEST_DIR/scx_lavd_smt_worker.c" \
  -o "$WORKER_BIN" -lbpf -lelf -lz

trap cleanup EXIT INT TERM

start_scheduler "$SCHED_LOG" "$TRACE_LOG"
if (( VERIFIER_FALLBACK_MODE )); then
  echo "Using verifier-friendly local fallback: --simple-enqueue"
fi

"$WORKER_BIN" \
  --label peer \
  --allowed-cpus "$CPU1" \
  --phase1-ms 5000 \
  --phase2-ms 5000 \
  --batch-iters "$PEER_BATCH_ITERS" \
  --yield-each-batch \
  --sched-ext \
  >"$PEER_LOG" 2>&1 &
PEER_PID=$!

sleep 0.5
peer_baseline_start="$(proc_ticks "$PEER_PID")"
sleep 1
peer_baseline_end="$(proc_ticks "$PEER_PID")"
peer_baseline_delta=$((peer_baseline_end - peer_baseline_start))

if (( peer_baseline_delta < 10 )); then
  fail "peer did not make enough baseline progress before SMT exclusivity ($peer_baseline_delta ticks)"
fi

"$WORKER_BIN" \
  --label exclusive \
  --allowed-cpus "$CPU0" \
  --map-path "$MAP_PATH" \
  --slice-ms 20 \
  --phase1-lat-cri "$PHASE1_LAT_CRI" \
  --phase2-lat-cri "$PHASE2_LAT_CRI" \
  --phase1-ms 3000 \
  --phase2-ms 4000 \
  --phase1-smt "$PHASE1_SMT" \
  --phase2-smt "$PHASE2_SMT" \
  --sched-ext \
  >"$EXCL_LOG" 2>&1 &
EXCL_PID=$!

sleep 0.3
peer_t0="$(proc_ticks "$PEER_PID")"
excl_t0="$(proc_ticks "$EXCL_PID")"
sleep 2.5
peer_t1="$(proc_ticks "$PEER_PID")"
excl_t1="$(proc_ticks "$EXCL_PID")"
sleep 2.5
peer_t2="$(proc_ticks "$PEER_PID")"
excl_t2="$(proc_ticks "$EXCL_PID")"

peer_phase1=$((peer_t1 - peer_t0))
peer_phase2=$((peer_t2 - peer_t1))
excl_phase1=$((excl_t1 - excl_t0))
excl_phase2=$((excl_t2 - excl_t1))

wait "$EXCL_PID"
EXCL_RUN_PID="$EXCL_PID"
EXCL_PID=""
PEER_RUN_PID="$PEER_PID"
kill "$PEER_PID" 2>/dev/null || true
wait "$PEER_PID" 2>/dev/null || true
PEER_PID=""
[[ "$(< /sys/kernel/sched_ext/state)" == "enabled" ]] \
  || fail "sched_ext disabled before primary SMT test completed"
assert_worker_log "$PEER_LOG" "peer" "$CPU1"
assert_worker_log "$EXCL_LOG" "exclusive" "$CPU0"
stop_scheduler

echo "peer baseline ticks: $peer_baseline_delta"
echo "peer phase1 ticks:   $peer_phase1"
echo "peer phase2 ticks:   $peer_phase2"
echo "exclusive phase1:    $excl_phase1"
echo "exclusive phase2:    $excl_phase2"

if (( excl_phase1 < 50 )); then
  fail "exclusive task did not make enough progress during SMT-exclusive phase ($excl_phase1 ticks)"
fi

grep -q "tld_smt_block:" "$TRACE_LOG" \
  || fail "missing tld_smt_block printk"
grep -q "tld_read: .*slice_ms val=" "$TRACE_LOG" \
  || fail "missing tld_read slice_ms printk"
grep -q "tld_read: .*lat_cri val=" "$TRACE_LOG" \
  || fail "missing tld_read lat_cri printk"
grep -q "pid=${EXCL_RUN_PID} .*smt_exclusive val=${EXPECT_PHASE1_RAW_SMT}" "$TRACE_LOG" \
  || fail "missing phase1 raw smt_exclusive=${EXPECT_PHASE1_RAW_SMT} trace for exclusive worker pid=${EXCL_RUN_PID}"
grep -q "pid=${EXCL_RUN_PID} .*smt_exclusive val=${EXPECT_PHASE2_RAW_SMT}" "$TRACE_LOG" \
  || fail "missing phase2 raw smt_exclusive=${EXPECT_PHASE2_RAW_SMT} trace for exclusive worker pid=${EXCL_RUN_PID}"
if [[ "$EXPECT_FALLBACK_LOG" == "1" ]]; then
  grep -q "tld_hint: pid=${EXCL_RUN_PID} smt_fallback lat_cri=${PHASE1_LAT_CRI}" "$TRACE_LOG" \
    || fail "missing smt_fallback trace for exclusive worker pid=${EXCL_RUN_PID}"
fi

block_count="$(rg -c "tld_smt_block: cpu=${CPU1} prev_pid=${PEER_RUN_PID}" "$TRACE_LOG" || true)"
if (( block_count < 20 )); then
  fail "expected repeated sibling dispatch blocking for peer pid=${PEER_RUN_PID}, saw block_count=${block_count}"
fi

echo "PASS: SMT exclusivity hint was decoded and sibling dispatch was blocked by LAVD"

if (( VERIFIER_FALLBACK_MODE )) && [[ "$RUN_LATENCY_IN_VERIFIER_FALLBACK" != "1" ]]; then
  echo "SKIP: latency benchmark disabled under verifier-friendly fallback; force with RUN_LATENCY_IN_VERIFIER_FALLBACK=1"
  exit 0
fi

control_latencies=()
exclusive_latencies=()

for run in $(seq 1 "$LATENCY_RUNS"); do
  control_ms="$(run_latency_trial 0 "$TRIAL_CONTROL_SMT" "$TRIAL_CONTROL_LAT_CRI" "$run")"
  exclusive_ms="$(run_latency_trial 1 "$TRIAL_EXCLUSIVE_SMT" "$TRIAL_EXCLUSIVE_LAT_CRI" "$run")"
  echo "latency run $run control_ms=$control_ms exclusive_ms=$exclusive_ms"
  control_latencies+=("$control_ms")
  exclusive_latencies+=("$exclusive_ms")
done

control_median="$(median "${control_latencies[@]}")"
exclusive_median="$(median "${exclusive_latencies[@]}")"
speedup_pct="$(awk -v control="$control_median" -v exclusive="$exclusive_median" '
  BEGIN {
    if (control == 0) {
      print "0.00";
    } else {
      printf "%.2f", ((control - exclusive) * 100.0) / control;
    }
  }')"

echo "latency control median ms:   $control_median"
echo "latency exclusive median ms: $exclusive_median"
echo "latency speedup percent:     $speedup_pct"

awk -v speedup="$speedup_pct" -v min="$MIN_SPEEDUP_PCT" '
  BEGIN {
    if (speedup + 0 < min + 0)
      exit 1;
  }' || fail "important worker did not speed up enough with SMT exclusivity (speedup=${speedup_pct}% threshold=${MIN_SPEEDUP_PCT}%)"

echo "PASS: SMT exclusivity measurably reduces latency for the important worker under sibling contention"
