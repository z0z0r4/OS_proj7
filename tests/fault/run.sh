#!/usr/bin/env bash
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
XV6_DIR="$ROOT_DIR/src"
LOG_DIR="$SCRIPT_DIR/logs"
TEST_ARCH="${TEST_ARCH:-riscv}"
LOG_SUFFIX=""
CASE_NAME="fault"
if [ "$TEST_ARCH" != "riscv" ]; then
  LOG_SUFFIX="-$TEST_ARCH"
  CASE_NAME="${TEST_ARCH}_error"
fi
RAW_LOG="$LOG_DIR/qemu-fault$LOG_SUFFIX.log"
RESULT_LOG="$LOG_DIR/result.log"

mkdir -p "$LOG_DIR"

if [ ! -f "$XV6_DIR/fs.img" ]; then
  echo "[fault] fs.img not found"
  echo "Use: make fault"
  exit 1
fi

echo "[fault] running QEMU command test"
# Keep incremental rebuilds consistent with the fault build.
export KEXTRA=-DP0_TEST_HOOKS
python3 "$ROOT_DIR/tests/runner/run_qemu_case.py" \
  --case "$CASE_NAME" \
  --xv6-dir "$XV6_DIR" \
  --commands "$SCRIPT_DIR/cmds.sh" \
  --log "$RAW_LOG" \
  --cpus 3 \
  --cmd-timeout 240 \
  --final-wait 120 \
  --prompt-wait \
  --step-wait 0.01 \
  --expect "fsfault: file system server is a separate process: OK" \
  --expect "fsfault: restart denied while service down: OK" \
  --expect "fsfault: no server created by unauthorized restart: OK" \
  --expect "fsfault: init restart still works: OK" \
  --expect "fsfault: init restarted the file system server: OK" \
  --expect "fsfault: pipes/fork work while file system is down: OK" \
  --expect "fsfault: stale open file is rejected: OK" \
  --expect "fsfault: data committed before the crash survived: OK" \
  --expect "fsfault: server restarted for stale cwd test: OK" \
  --expect "fsfault: stale cwd rejects relative open: OK" \
  --expect "fsfault: stale cwd rejects relative mkdir: OK" \
  --expect "fsfault: stale cwd rejects relative unlink: OK" \
  --expect "fsfault: stale cwd rejects relative link source: OK" \
  --expect "fsfault: stale cwd rejects relative link target: OK" \
  --expect "fsfault: stale cwd rejects relative mknod: OK" \
  --expect "fsfault: stale cwd rejects relative mount: OK" \
  --expect "fsfault: absolute open works with stale cwd: OK" \
  --expect "fsfault: absolute read works with stale cwd: OK" \
  --expect "fsfault: absolute link works with stale cwd: OK" \
  --expect "fsfault: absolute mount works with stale cwd: OK" \
  --expect "fsfault: absolute path into mount works: OK" \
  --expect "fsfault: stale cwd rejects relative umount: OK" \
  --expect "fsfault: fork child does not inherit a root cwd: OK" \
  --expect "fsfault: fork child absolute access works: OK" \
  --expect "fsfault: fork child recovers with an absolute chdir: OK" \
  --expect "fsfault: fork child cwd checks passed: OK" \
  --expect "fsfault: stale cwd rejects relative exec: OK" \
  --expect "fsfault: bare-name exec still falls back to root: OK" \
  --expect "fsfault: stale cwd rejects relative chdir: OK" \
  --expect "fsfault: absolute chdir restores relative access: OK" \
  --expect "fsfault: relative link works after recovery: OK" \
  --expect "fsfault: absolute umount succeeds: OK" \
  --expect "fsfault: rejected relative umount had no effect: OK" \
  --expect "fsfault: recovered from a crash inside the server: OK" \
  --expect "fsfault: survived 6 server crashes under load: OK" \
  --expect "fsfault: remount ext2 after restart: OK" \
  --expect "fsfault: legal xfer from client works: OK" \
  --expect "fsfault: legal xfer to client works: OK" \
  --expect "fsfault: open for xfer direction test: OK" \
  --expect "fsfault: legal xfer before direction test: OK" \
  --expect "fsfault: xfer direction hook armed: OK" \
  --expect "fsfault: invalid xfer directions rejected without copying: OK" \
  --expect "fsfault: legal xfer resumes after rejection: OK" \
  --expect "fsfault: xfer range round 1 armed: OK" \
  --expect "fsfault: xfer range inside grant and both directions work: OK" \
  --expect "fsfault: xfer range round 2 armed: OK" \
  --expect "fsfault: xfer range out-of-grant and caddr overflow rejected: OK" \
  --expect "fsfault: xfer range round 3 armed: OK" \
  --expect "fsfault: wrapping grant safely rejected: OK" \
  --expect "fsfault: xfer range round 4 armed: OK" \
  --expect "fsfault: xfer invalid and wide lengths rejected: OK" \
  --expect "fsfault: xfer range round 5 armed: OK" \
  --expect "fsfault: server buffer bounds enforced: OK" \
  --expect "fsfault: zero-length read leaves grant untouched: OK" \
  --expect "fsfault: genrace open restarted the server: OK" \
  --expect "fsfault: genrace open handle stays usable after the restart: OK" \
  --expect "fsfault: genrace chdir restarted the server: OK" \
  --expect "fsfault: genrace relative open survives the restart during chdir: OK" \
  --expect "fsfault: genrace fork child inherits a valid cwd after the restart: OK" \
  --expect "fsfault: genrace exec restarted the server: OK" \
  --expect "fsfault: genrace exec survives the restart during open: OK" \
  --expect-count "fsfault: " 87 \
  --expect "FSFAULT_DONE" \
  --expect "FAULT_PATH_DONE" \
  --reject ": FAIL" \
  --reject "FSFAULT_FAIL" \
  --reject "panic:" \
  --reject "kerneltrap"
test_status=$?

if [ "$test_status" -eq 0 ]; then
  result="PASS"
else
  result="FAIL"
fi

{
  echo "Result: $result"
  echo "Test Status: $test_status"
  echo "Command File: $SCRIPT_DIR/cmds.sh"
  echo "Raw Log: $RAW_LOG"
} > "$RESULT_LOG"

echo "[fault] result: $result"
echo "[fault] logs: $LOG_DIR"

if [ "$result" != "PASS" ]; then
  exit 1
fi
