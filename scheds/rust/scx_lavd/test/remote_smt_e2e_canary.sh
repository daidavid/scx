#!/usr/bin/env bash

set -euo pipefail

CANARY_VERSION="2026-04-15b"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")"/../../../.. && pwd)"
TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOST="${HOST:?set HOST=<target-host>}"
CONTROL_HOST="${CONTROL_HOST:-twshared67495.02.vll3}"
CONTROL_BIN="${CONTROL_BIN:-/root/scx_lavd}"
LOCAL_SCX_LAVD_BIN="${LOCAL_SCX_LAVD_BIN:-$ROOT_DIR/target/release/scx_lavd}"
LOCAL_CONTROL_BIN="${LOCAL_CONTROL_BIN:-/tmp/scx_lavd_control_${CONTROL_HOST//[^A-Za-z0-9._-]/_}}"
LOCAL_WORKER_BIN="${LOCAL_WORKER_BIN:-/tmp/scx_lavd_smt_worker_${HOST//[^A-Za-z0-9._-]/_}}"
REMOTE_DIR="${REMOTE_DIR:-/var/tmp/scx_lavd_smt_e2e}"
REMOTE_BIN="${REMOTE_BIN:-/root/scx_lavd}"
REMOTE_BIN_BAK="${REMOTE_BIN_BAK:-$REMOTE_DIR/scx_lavd.prev}"
REMOTE_LOG="${REMOTE_LOG:-/tmp/scx_lavd.log}"
REMOTE_TMP_DIR="${REMOTE_TMP_DIR:-$REMOTE_DIR/artifacts}"
REMOTE_MAP="${REMOTE_MAP:-/run/wds/shared_bpf/scx_tld_hint_map}"
REMOTE_STAGE_DIR="${REMOTE_STAGE_DIR:-/tmp/${USER}_scx_lavd_smt_e2e_stage}"
REMOTE_TEST_SCRIPT="$REMOTE_DIR/test_smt_exclusive.sh"
REMOTE_WRAPPER="$REMOTE_DIR/t"
REMOTE_WORKER_BIN="$REMOTE_DIR/scx_lavd_smt_worker"
REMOTE_TLD_HDR="$REMOTE_DIR/task_local_data_user.h"
REMOTE_SHORTCUT="/tmp/t"
SMT_LAT_CRI_THRESHOLD="${SMT_LAT_CRI_THRESHOLD:-16000}"
CPU_PAIR="${CPU_PAIR:-}"
EXPECT_FIRST_CHUNK_SLICE_HEX="${EXPECT_FIRST_CHUNK_SLICE_HEX:-14}"
TRACE_PIPE="${TRACE_PIPE:-}"
STEP_SLEEP_SECS="${STEP_SLEEP_SECS:-1}"
POST_START_WAIT_SECS="${POST_START_WAIT_SECS:-10}"
VERBOSE_REMOTE_PHASES="${VERBOSE_REMOTE_PHASES:-0}"

cleanup() {
  rm -f "$LOCAL_CONTROL_BIN"
  rm -f "$LOCAL_WORKER_BIN"
}

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

sanitize_output() {
  sed '
    /^Meta authorized users only\. Usage is subject to monitoring and recording\.$/d
    /^An action was blocked on this server based on a security policy!$/d
    /^Bunnylol `scuba bpfjailer_enforce` for more details$/d
    /^Role: .*$/d
    /^Command: .*$/d
    /^User .*$/d
    /^Enforcer: .*$/d
    /^If you believe this action was in error, post in .*$/d
    /^If the matter is urgent, contact the `bpfj` oncall$/d
    /^AI Agent Detected: .*$/d
    /^Sush2 has detected that it is being invoked from within an AI agent$/d
    /^environment\. Some operations may be restricted\.$/d
    /^For more details: https:\/\/fburl\.com\/agent-ssh$/d
    /^$/d
  '
}

check_step() {
  local label="$1"
  shift
  local tmp_output
  local rc

  printf '[check] %s: ' "$label"
  tmp_output="$(mktemp /tmp/scx_lavd_check.XXXXXX)"
  set +e
  "$@" >"$tmp_output" 2>&1
  rc=$?
  set -e
  if (( rc == 0 )); then
    echo "yes"
  else
    echo "no"
    sanitize_output <"$tmp_output" >&2 || true
    rm -f "$tmp_output"
    fail "$label"
  fi
  rm -f "$tmp_output"
  pause_step
}

pause_step() {
  sleep "$STEP_SLEEP_SECS"
}

log_step() {
  echo "[local] $*"
  pause_step
}

require_local_file() {
  local path="$1"

  [[ -x "$path" ]] || fail "missing executable: $path"
}

require_local_cmd() {
  local cmd="$1"

  command -v "$cmd" >/dev/null 2>&1 || fail "missing required command: $cmd"
}

copy_to_remote_stage() {
  local local_path="$1"
  local remote_name="$2"

  sush2 "$HOST" "cat > '$REMOTE_STAGE_DIR/$remote_name'" < "$local_path"
}

stream_to_remote_stage() {
  local local_path="$1"
  local remote_name="$2"

  sush2 "$HOST" "cat > '$REMOTE_STAGE_DIR/$remote_name'" < "$local_path"
}

stream_from_remote_root() {
  local remote_host="$1"
  local remote_root_path="$2"
  local local_path="$3"

  sush2 "$remote_host" "sudo -n cat '$remote_root_path'" > "$local_path"
}

install_remote_stage_file() {
  local remote_name="$1"
  local remote_dest="$2"
  local mode="$3"

  sush2 "$HOST" "sudo -n install -m '$mode' '$REMOTE_STAGE_DIR/$remote_name' '$remote_dest'"
}

install_test_assets() {
  install_remote_stage_file "test_smt_exclusive.sh" "$REMOTE_TEST_SCRIPT" 755
  install_remote_stage_file "t" "$REMOTE_WRAPPER" 755
  install_remote_stage_file "task_local_data_user.h" "$REMOTE_TLD_HDR" 644
  install_remote_stage_file "scx_lavd_smt_worker" "$REMOTE_WORKER_BIN" 755
  sush2 "$HOST" "sudo -n chmod 755 '$REMOTE_TEST_SCRIPT' '$REMOTE_WRAPPER' '$REMOTE_WORKER_BIN' && sudo -n ln -sf '$REMOTE_WRAPPER' '$REMOTE_SHORTCUT'"
}

copy_static_assets() {
  require_local_cmd cc

  check_step "sush2 target host reachable" sush2 "$HOST" "true"
  log_step "building worker binary locally"
  check_step "local worker build complete" \
    cc -O2 -Wall -Wextra -I"$TEST_DIR" \
    "$TEST_DIR/scx_lavd_smt_worker.c" \
    -o "$LOCAL_WORKER_BIN" -lbpf -lelf -lz

  log_step "preparing remote staging directory on $HOST"
  check_step "remote staging dir prepared with sush2" \
    sush2 "$HOST" "rm -rf '$REMOTE_STAGE_DIR' && mkdir -p '$REMOTE_STAGE_DIR'"

  log_step "preparing remote directory on $HOST"
  check_step "remote artifact dir prepared with sush2" \
    sush2 "$HOST" \
      "sudo -n mkdir -p '$REMOTE_DIR' '$REMOTE_TMP_DIR' && \
       sudo -n chown -R \$(id -u):\$(id -g) '$REMOTE_DIR'"

  log_step "streaming test script to remote stage via sush2"
  check_step "sush2 test script stream complete" \
    copy_to_remote_stage "$TEST_DIR/test_smt_exclusive.sh" "test_smt_exclusive.sh"
  check_step "test script visible on host" \
    sush2 "$HOST" "test -s '$REMOTE_STAGE_DIR/test_smt_exclusive.sh'"

  log_step "streaming wrapper to remote stage via sush2"
  check_step "sush2 wrapper stream complete" \
    copy_to_remote_stage "$TEST_DIR/t" "t"
  check_step "wrapper visible on host" \
    sush2 "$HOST" "test -s '$REMOTE_STAGE_DIR/t'"

  log_step "streaming TLD header to remote stage via sush2"
  check_step "sush2 TLD header stream complete" \
    copy_to_remote_stage "$TEST_DIR/task_local_data_user.h" "task_local_data_user.h"
  check_step "TLD header visible on host" \
    sush2 "$HOST" "test -s '$REMOTE_STAGE_DIR/task_local_data_user.h'"

  log_step "streaming worker binary to remote stage via sush2"
  check_step "sush2 worker binary stream complete" \
    stream_to_remote_stage "$LOCAL_WORKER_BIN" "scx_lavd_smt_worker"
  check_step "worker binary visible on host" \
    sush2 "$HOST" "test -s '$REMOTE_STAGE_DIR/scx_lavd_smt_worker'"

  log_step "installing staged test assets with sush2"
  check_step "test assets installed with sush2" \
    install_test_assets
}

deploy_remote_binary() {
  local local_bin="$1"
  local label="$2"
  local stage_name="scx_lavd.${label}"

  log_step "streaming $label binary to remote stage via sush2"
  check_step "sush2 $label LAVD binary stream complete" \
    stream_to_remote_stage "$local_bin" "$stage_name"
  check_step "$label LAVD binary visible on host" \
    sush2 "$HOST" "test -s '$REMOTE_STAGE_DIR/$stage_name'"

  log_step "installing $label binary to $REMOTE_BIN with sush2"
  check_step "$label LAVD install complete with sush2" \
    sush2 "$HOST" \
      "sudo -n install -m 755 '$REMOTE_STAGE_DIR/$stage_name' '$REMOTE_BIN.new' && \
       if sudo -n test -x '$REMOTE_BIN'; then sudo -n cp '$REMOTE_BIN' '$REMOTE_BIN_BAK'; fi && \
       sudo -n mv '$REMOTE_BIN.new' '$REMOTE_BIN'"
}

fetch_control_binary() {
  log_step "staging clean control binary on $CONTROL_HOST with sush2"
  check_step "control host reachable via sush2" sush2 "$CONTROL_HOST" "true"
  check_step "clean control binary streamed with sush2" \
    stream_from_remote_root "$CONTROL_HOST" "$CONTROL_BIN" "$LOCAL_CONTROL_BIN"
  chmod 755 "$LOCAL_CONTROL_BIN"
  echo "[local] control sha256: $(sha256sum "$LOCAL_CONTROL_BIN" | awk '{print $1}')"
}

run_remote_phase() {
  local phase_label="$1"
  local expect_smt="$2"
  local tmp_output
  local rc=0

  log_step "running phase '$phase_label' on $HOST"
  tmp_output="$(mktemp /tmp/scx_lavd_remote_phase.${phase_label}.XXXXXX)"
  printf '[check] %s LAVD phase complete: ' "$phase_label"
  set +e
  sush2 "$HOST" \
    "PHASE_LABEL=$(printf '%q' "$phase_label") \
     EXPECT_SMT=$(printf '%q' "$expect_smt") \
     REMOTE_DIR=$(printf '%q' "$REMOTE_DIR") \
     REMOTE_BIN=$(printf '%q' "$REMOTE_BIN") \
     REMOTE_LOG=$(printf '%q' "$REMOTE_LOG") \
     REMOTE_TMP_DIR=$(printf '%q' "$REMOTE_TMP_DIR") \
     REMOTE_MAP=$(printf '%q' "$REMOTE_MAP") \
     REMOTE_SHORTCUT=$(printf '%q' "$REMOTE_SHORTCUT") \
     REMOTE_WORKER_BIN=$(printf '%q' "$REMOTE_WORKER_BIN") \
     SMT_LAT_CRI_THRESHOLD=$(printf '%q' "$SMT_LAT_CRI_THRESHOLD") \
     CPU_PAIR=$(printf '%q' "$CPU_PAIR") \
     EXPECT_FIRST_CHUNK_SLICE_HEX=$(printf '%q' "$EXPECT_FIRST_CHUNK_SLICE_HEX") \
     TRACE_PIPE=$(printf '%q' "$TRACE_PIPE") \
     POST_START_WAIT_SECS=$(printf '%q' "$POST_START_WAIT_SECS") \
     bash -s" <<'EOF' >"$tmp_output" 2>&1
  rc=$?
  set -e
set -euo pipefail

fail() {
  echo "FAIL[$PHASE_LABEL]: $*" >&2
  exit 1
}

dump_key_bpfprints() {
  echo "[remote] phase=$PHASE_LABEL key bpfprints"
  if [[ -n "${peer_run_pid:-}" || -n "${exclusive_run_pid:-}" ]]; then
    grep -E 'tld_smt_apply|tld_smt_block|tld_read:|tld_hint:' "$trace_log" | \
      grep -E "pid=${peer_run_pid:-0}|pid=${exclusive_run_pid:-0}|tld_smt_block" | \
      tail -n 40 || true
  else
    grep -E 'tld_smt_apply|tld_smt_block|tld_read:|tld_hint:' "$trace_log" | tail -n 24 || true
  fi
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

dump_startup_debug() {
  local pid="${1:-}"

  echo "[remote] phase=$PHASE_LABEL startup-debug"
  echo "[remote] binary version: $("$REMOTE_BIN" --version 2>/dev/null || true)"
  echo "[remote] binary help flags:"
  "$REMOTE_BIN" --help 2>/dev/null | grep -E -- '--task-hint-map|--log-level|--partial|--smt|--no-wake-sync|--no-execve-migration-hooks' || true
  echo "[remote] sched_ext state: $(cat /sys/kernel/sched_ext/state 2>/dev/null || echo missing)"
  echo "[remote] root ops: $(cat /sys/kernel/sched_ext/root/ops 2>/dev/null || echo missing)"
  echo "[remote] sched_ext root tree:"
  ls -l /sys/kernel/sched_ext/root 2>/dev/null || true
  echo "[remote] map exists: $(if [[ -e "$REMOTE_MAP" ]]; then echo yes; else echo no; fi)"
  echo "[remote] map details:"
  ls -l "$REMOTE_MAP" 2>/dev/null || true
  stat -Lc '%n inode=%i mode=%a uid=%u gid=%g' "$REMOTE_MAP" 2>/dev/null || true
  echo "[remote] pinned map:"
  sudo -n bpftool map show pinned "$REMOTE_MAP" 2>/dev/null || true
  echo "[remote] remote log: $REMOTE_LOG"
  if [[ -n "$pid" && -d "/proc/$pid" ]]; then
    echo "[remote] pid: $pid"
    echo "[remote] cmdline: $(tr '\0' ' ' < /proc/$pid/cmdline 2>/dev/null || true)"
    echo "[remote] exe: $(readlink -f /proc/$pid/exe 2>/dev/null || true)"
  else
    echo "[remote] pid: missing"
  fi
  echo "[remote] pgrep -a scx_lavd:"
  pgrep -a scx_lavd 2>/dev/null || true
  echo "[remote] bpftool prog tail:"
  sudo -n bpftool prog show 2>/dev/null | tail -n 80 || true
  echo "[remote] last log lines"
  tail -n 60 "$REMOTE_LOG" 2>/dev/null || true
  echo "[remote] kernel tail"
  sudo -n dmesg -T 2>/dev/null | tail -n 80 || true
}

classify_startup_failure() {
  if grep -q "BPF program is too large" "$REMOTE_LOG" 2>/dev/null; then
    echo "candidate verifier instruction-limit cliff on prod-equivalent args"
    return
  fi
  if grep -q "pointer arithmetic on mem_or_null prohibited" "$REMOTE_LOG" 2>/dev/null; then
    echo "candidate verifier failure: kernel rejected nullable pointer arithmetic in candidate BPF"
    return
  fi
  if grep -q "failed to load BPF program" "$REMOTE_LOG" 2>/dev/null; then
    echo "candidate BPF load failure"
    return
  fi
  if grep -q "Permission denied (os error 13)" "$REMOTE_LOG" 2>/dev/null; then
    echo "candidate BPF verifier returned -EACCES"
    return
  fi
  echo "candidate startup failure: unknown"
}

find_trace_pipe() {
  local candidate

  if [[ -n "${TRACE_PIPE:-}" ]] && sudo -n test -r "${TRACE_PIPE:-}"; then
    echo "$TRACE_PIPE"
    return 0
  fi

  for candidate in /sys/kernel/tracing/trace_pipe /sys/kernel/debug/tracing/trace_pipe; do
    if sudo -n test -r "$candidate"; then
      echo "$candidate"
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

phase_tmp_dir="$REMOTE_TMP_DIR/$PHASE_LABEL"
trace_log="$phase_tmp_dir/trace.log"
peer_log="$phase_tmp_dir/peer.log"
excl_log="$phase_tmp_dir/exclusive.log"
sudo -n true
trace_pipe="$(find_trace_pipe)" || fail "trace_pipe is required"

mkdir -p "$phase_tmp_dir"
rm -f "$phase_tmp_dir"/*

if [[ -n "$CPU_PAIR" ]]; then
  pair="$CPU_PAIR"
else
  pair="$(find_sibling_pair)" || fail "failed to find SMT sibling pair"
fi

IFS=, read -r cpu0 cpu1 <<< "$pair"
[[ -n "$cpu0" && -n "$cpu1" ]] || fail "failed to parse CPU pair '$pair'"
[[ -x "$REMOTE_WORKER_BIN" ]] || fail "missing remote worker binary: $REMOTE_WORKER_BIN"

echo "[remote] phase=$PHASE_LABEL host=$(hostname -f 2>/dev/null || hostname)"
echo "[remote] phase=$PHASE_LABEL cpu_pair=$pair"
echo "[remote] phase=$PHASE_LABEL scx_lavd sha256=$(sha256sum "$REMOTE_BIN" | awk '{print $1}')"
echo "[remote] phase=$PHASE_LABEL one-liner=$REMOTE_SHORTCUT"

# Exact runbook startup from P2270794745 / P2270820228.
sudo -n pkill -f scx_lavd 2>/dev/null || true
sleep 2
sudo -n rm -f "$REMOTE_MAP"
sudo -n nohup "$REMOTE_BIN" --performance --log-level=trace --task-hint-map "$REMOTE_MAP" >"$REMOTE_LOG" 2>&1 </dev/null &
disown || true
sleep 3
sudo -n chmod 666 "$REMOTE_MAP" 2>/dev/null || true

lavd_pid=""
state=""
for _ in $(seq 1 "$POST_START_WAIT_SECS"); do
  lavd_pid="$(pgrep -xo scx_lavd 2>/dev/null || true)"
  state="$(cat /sys/kernel/sched_ext/state 2>/dev/null || true)"
  if [[ "$state" == "enabled" && -e "$REMOTE_MAP" && -n "$lavd_pid" ]]; then
    break
  fi
  sleep 1
done

ops="$(cat /sys/kernel/sched_ext/root/ops 2>/dev/null || true)"
cmdline=""
if [[ -n "$lavd_pid" && -d "/proc/$lavd_pid" ]]; then
  cmdline="$(tr '\0' ' ' < /proc/$lavd_pid/cmdline 2>/dev/null || true)"
fi

if [[ "$state" != "enabled" ]]; then
  dump_startup_debug "$lavd_pid"
  fail "sched_ext did not enable ($(classify_startup_failure))"
fi
if [[ ! -e "$REMOTE_MAP" ]]; then
  dump_startup_debug "$lavd_pid"
  fail "task hint map missing after exact startup"
fi
grep -q -- '--task-hint-map' <<<"$cmdline" || fail "cmdline missing --task-hint-map"
grep -q -- '--log-level=trace' <<<"$cmdline" || fail "cmdline missing --log-level=trace"

echo "[remote] phase=$PHASE_LABEL sched_ext=$state ops=$ops"
echo "[remote] phase=$PHASE_LABEL cmdline=$cmdline"

sudo -n sh -c ": > '$(dirname "$trace_pipe")/trace'"
sudo -n sh -c "echo 1 > '$(dirname "$trace_pipe")/tracing_on'"
sudo -n cat "$trace_pipe" > "$trace_log" &
trace_pid=$!

"$REMOTE_WORKER_BIN" \
  --label peer \
  --allowed-cpus "$cpu1" \
  --phase1-ms 1500 \
  --phase2-ms 7500 \
  --batch-iters 100000 \
  --yield-each-batch \
  --sched-ext \
  >"$peer_log" 2>&1 &
peer_pid=$!

sleep 0.5

"$REMOTE_WORKER_BIN" \
  --label exclusive \
  --allowed-cpus "$cpu0" \
  --map-path "$REMOTE_MAP" \
  --slice-ms 20 \
  --phase1-lat-cri "$SMT_LAT_CRI_THRESHOLD" \
  --phase2-lat-cri 128 \
  --phase1-ms 3000 \
  --phase2-ms 4000 \
  --phase1-smt "$EXPECT_SMT" \
  --phase2-smt 0 \
  --sched-ext \
  >"$excl_log" 2>&1 &
excl_pid=$!

wait "$excl_pid"
kill "$peer_pid" 2>/dev/null || true
wait "$peer_pid" 2>/dev/null || true
sleep 1
kill "$trace_pid" 2>/dev/null || true
wait "$trace_pid" 2>/dev/null || true
sudo -n sh -c "echo 0 > '$(dirname "$trace_pipe")/tracing_on'" || true

[[ -s "$trace_log" ]] || fail "missing trace log at $trace_log"
[[ "$(cat /sys/kernel/sched_ext/state 2>/dev/null || true)" == "enabled" ]] \
  || fail "sched_ext disabled before remote phase completed"
assert_worker_log "$peer_log" "peer" "$cpu1"
assert_worker_log "$excl_log" "exclusive" "$cpu0"
peer_run_pid="$(sed -n 's/^peer pid=\([0-9]\+\).*/\1/p' "$peer_log" | head -n 1)"
exclusive_run_pid="$(sed -n 's/^exclusive pid=\([0-9]\+\).*/\1/p' "$excl_log" | head -n 1)"
[[ -n "$peer_run_pid" ]] || fail "failed to parse peer pid from $peer_log"
[[ -n "$exclusive_run_pid" ]] || fail "failed to parse exclusive pid from $excl_log"
expected_lat_hex="$(printf '%x' "$SMT_LAT_CRI_THRESHOLD")"

if [[ "$EXPECT_SMT" == "1" ]]; then
  grep -Eq "pid=${exclusive_run_pid} .*slice_ms val=(0x)?${EXPECT_FIRST_CHUNK_SLICE_HEX}\\>" "$trace_log" \
    || { dump_key_bpfprints; fail "missing candidate slice_ms val=${EXPECT_FIRST_CHUNK_SLICE_HEX} for pid=${exclusive_run_pid}"; }
  grep -Eq "pid=${exclusive_run_pid} .*lat_cri val=(0x)?${expected_lat_hex}\\>" "$trace_log" \
    || { dump_key_bpfprints; fail "missing candidate lat_cri val=${expected_lat_hex} for pid=${exclusive_run_pid}"; }
  grep -Eq "pid=${exclusive_run_pid} .*smt_exclusive val=1\\>" "$trace_log" \
    || { dump_key_bpfprints; fail "missing candidate smt_exclusive val=1 for pid=${exclusive_run_pid}"; }
  grep -q "tld_smt_block: cpu=${cpu1} prev_pid=${peer_run_pid}" "$trace_log" \
    || { dump_key_bpfprints; fail "missing candidate tld_smt_block for peer pid=${peer_run_pid} on cpu=${cpu1}"; }
  echo "[remote] phase=$PHASE_LABEL verified pid=$exclusive_run_pid slice_ms=${EXPECT_FIRST_CHUNK_SLICE_HEX} lat_cri=${expected_lat_hex} smt_exclusive=1 blocked peer pid=$peer_run_pid on cpu=$cpu1"
else
  grep -Eq "pid=${exclusive_run_pid} .*slice_ms val=(0x)?${EXPECT_FIRST_CHUNK_SLICE_HEX}\\>" "$trace_log" \
    || { dump_key_bpfprints; fail "missing first chunk slice_ms val=${EXPECT_FIRST_CHUNK_SLICE_HEX} for pid=${exclusive_run_pid}"; }
  grep -Eq "pid=${exclusive_run_pid} .*lat_cri val=(0x)?${expected_lat_hex}\\>" "$trace_log" \
    || { dump_key_bpfprints; fail "missing first chunk lat_cri val=${expected_lat_hex} for pid=${exclusive_run_pid}"; }
  echo "[remote] phase=$PHASE_LABEL verified pid=$exclusive_run_pid slice_ms=${EXPECT_FIRST_CHUNK_SLICE_HEX} lat_cri=${expected_lat_hex}"
fi

dump_key_bpfprints
echo
echo "[remote] phase=$PHASE_LABEL artifacts"
echo "  $REMOTE_LOG"
echo "  $trace_log"
echo "  $peer_log"
echo "  $excl_log"
echo "[remote] phase=$PHASE_LABEL status=PASS"
exit 0
EOF
  if grep -q "\\[remote\\] phase=$phase_label status=PASS" "$tmp_output"; then
    echo "yes"
    if [[ "$VERBOSE_REMOTE_PHASES" == "1" ]]; then
      sanitize_output <"$tmp_output"
    else
      sanitize_output <"$tmp_output" | grep -E "^\\[remote\\] phase=${phase_label} (host=|cpu_pair=|scx_lavd sha256=|sched_ext=|cmdline=|verified |artifacts|status=PASS)" || true
      sanitize_output <"$tmp_output" | grep -E '^  /' || true
    fi
  else
    echo "no"
    sanitize_output <"$tmp_output" | sed -n '1,160p' >&2
    rm -f "$tmp_output"
    fail "$phase_label LAVD phase complete (rc=$rc)"
  fi
  rm -f "$tmp_output"
  pause_step
}

trap cleanup EXIT

require_local_file "$LOCAL_SCX_LAVD_BIN"

for cmd in sush2 suscp; do
  command -v "$cmd" >/dev/null 2>&1 || fail "missing required command: $cmd"
done

echo "[local] target host: $HOST"
echo "[local] control host: $CONTROL_HOST"
echo "[local] candidate binary: $LOCAL_SCX_LAVD_BIN"
echo "[local] remote binary path: $REMOTE_BIN"
echo "[local] remote tmp dir: $REMOTE_TMP_DIR"
echo "[local] remote stage dir: $REMOTE_STAGE_DIR"
echo "[local] remote map: $REMOTE_MAP"
echo "[local] expected first chunk slice hex: $EXPECT_FIRST_CHUNK_SLICE_HEX"
echo "[local] startup runbook source: P2270794745 / P2270820228"
echo "[local] step sleep seconds: $STEP_SLEEP_SECS"
echo "[local] post-start wait seconds: $POST_START_WAIT_SECS"
echo "[local] canary version: $CANARY_VERSION"

copy_static_assets
fetch_control_binary
deploy_remote_binary "$LOCAL_CONTROL_BIN" "control_preflight"
run_remote_phase "control_preflight" "0"

deploy_remote_binary "$LOCAL_SCX_LAVD_BIN" "candidate"
run_remote_phase "candidate" "1"

deploy_remote_binary "$LOCAL_CONTROL_BIN" "control"
run_remote_phase "control" "0"

echo "[local] PASS: A/B canary succeeded"
echo "[local] preflight: clean binary from $CONTROL_HOST attached and emitted first chunk slice_ms val=$EXPECT_FIRST_CHUNK_SLICE_HEX"
echo "[local] A: candidate binary emitted SMT toggle bpfprints after exact runbook startup"
echo "[local] B: clean binary from $CONTROL_HOST emitted first chunk slice_ms val=$EXPECT_FIRST_CHUNK_SLICE_HEX after exact runbook startup"
