#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
multicore=()
simulator_backend=()
tap="${TRIBE_INTERACTIVE_TAP:-tap-tribe}"
guest_address="${TRIBE_INTERACTIVE_GUEST_ADDRESS:-192.168.76.2}"
host_address="${TRIBE_INTERACTIVE_HOST_ADDRESS:-192.168.76.1}"
guest_port="${TRIBE_INTERACTIVE_SSH_PORT:-22}"
wall_timeout="${TRIBE_INTERACTIVE_SSH_TIMEOUT:-3600}"
dropbear_identity="${TRIBE_INTERACTIVE_DROPBEAR_IDENTITY:-$root/build/tests/busybox/dropbear-host/mikos_ssh_key.dropbear}"
early_client="${TRIBE_INTERACTIVE_SSH_EARLY_CLIENT:-0}"
late_diagnostics="${TRIBE_INTERACTIVE_SSH_DIAGNOSTICS:-0}"
uart_preempt_probe="${TRIBE_INTERACTIVE_SSH_UART_PREEMPT_PROBE:-0}"
pty_only="${TRIBE_INTERACTIVE_SSH_PTY_ONLY:-0}"
quiet="${TRIBE_INTERACTIVE_SSH_QUIET:-0}"
pcap="${TRIBE_INTERACTIVE_SSH_PCAP:-$root/build/tribe-interactive-ssh.pcap}"
eth_trace="${TRIBE_INTERACTIVE_SSH_ETH_TRACE:-$root/build/tribe-interactive-ssh.eth.log}"

if [[ "$quiet" != 0 && "$quiet" != 1 ]]; then
  echo "TRIBE_INTERACTIVE_SSH_QUIET must be 0 or 1" >&2
  exit 2
fi

if [[ "${MIKOS_TRIBE_SSH_NETNS_ENTERED:-0}" != 1 &&
      -z "${TRIBE_ETH_TAP_SOCKET:-}" ]]; then
  exec unshare -Urn env MIKOS_TRIBE_SSH_NETNS_ENTERED=1 "$0" "$@"
fi

while [[ $# -gt 0 ]]; do
  case "$1" in
    --multicore)
      multicore=(--multicore)
      ;;
    --verilator)
      simulator_backend=(--verilator)
      ;;
    -h|--help)
      echo "usage: $0 [--multicore] [--verilator]" >&2
      exit 0
      ;;
    *)
      echo "usage: $0 [--multicore] [--verilator]" >&2
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
  shift
done
for required in "$dropbear_identity" \
                "$root/build/tests/busybox/rootfs.ext4" \
                "$root/build/tests/qemu/ethgig_tap"; do
  if [[ ! -f "$required" ]]; then
    echo "missing required SSH test input: $required" >&2
    exit 1
  fi
done

runtime="$(mktemp -d /tmp/mikos-interactive-ssh.XXXXXX)"
input="$runtime/uart.in"
log="$runtime/uart.log"
ssh_log="$runtime/ssh.log"
bridge_log="$runtime/bridge.log"
rootfs="$runtime/rootfs.ext4"
tap_socket="${TRIBE_ETH_TAP_SOCKET:-$runtime/tribe-ethgig.sock}"
cp --reflink=auto "$root/build/tests/busybox/rootfs.ext4" "$rootfs"
mkfifo "$input"
exec 3<>"$input"
: >"$log"
if [[ "$quiet" != 1 ]]; then
  # Keep the file as the authoritative source for marker checks while making
  # hour-long native C++ runs observable from the invoking terminal.
  tail -s 0.1 -n +1 -f "$log" &
  log_tail_pid=$!
fi

cleanup() {
  if [[ -n "${ssh_input_fd:-}" ]]; then
    exec {ssh_input_fd}>&-
    ssh_input_fd=""
  fi
  if [[ -n "${ssh_pid:-}" ]]; then
    kill -CONT "${ssh_pause_target:-$ssh_pid}" 2>/dev/null || true
    kill -CONT "$ssh_pid" 2>/dev/null || true
    kill "${ssh_pause_target:-$ssh_pid}" 2>/dev/null || true
    kill "$ssh_pid" 2>/dev/null || true
    wait "$ssh_pid" 2>/dev/null || true
  fi
  if [[ -n "${simulator_pid:-}" ]]; then
    kill "$simulator_pid" 2>/dev/null || true
    wait "$simulator_pid" 2>/dev/null || true
  fi
  if [[ -n "${bridge_pid:-}" ]]; then
    kill "$bridge_pid" 2>/dev/null || true
    wait "$bridge_pid" 2>/dev/null || true
  fi
  if [[ -n "${log_tail_pid:-}" ]]; then
    kill "$log_tail_pid" 2>/dev/null || true
    wait "$log_tail_pid" 2>/dev/null || true
  fi
  exec 3>&-
  rm -r "$runtime"
}
trap cleanup EXIT

if [[ ! -S "$tap_socket" ]]; then
  MIKOS_ETH_TAP_PCAP="$pcap" \
    "$root/build/tests/qemu/ethgig_tap" --tap "$tap" \
    --address "$host_address/24" --socket "$tap_socket" \
    >"$bridge_log" 2>&1 &
  bridge_pid=$!
  for _ in $(seq 1 200); do
    [[ -S "$tap_socket" ]] && break
    if ! kill -0 "$bridge_pid" 2>/dev/null; then
      cat "$bridge_log" >&2
      echo "failed to start private Tribe TAP bridge" >&2
      exit 1
    fi
    sleep 0.02
  done
fi
if [[ ! -S "$tap_socket" ]] || ! ip link show dev "$tap" >/dev/null 2>&1; then
  cat "$bridge_log" >&2 2>/dev/null || true
  echo "Tribe TAP bridge did not become ready" >&2
  exit 1
fi
if [[ -n "${bridge_pid:-}" ]]; then
  # A private TAP needs only the test IPv4 subnet. Suppress automatic IPv6
  # multicast traffic, which otherwise competes with SSH in Tribe's bounded
  # real-time-to-simulated-time RX queue.
  sysctl -q -w "net.ipv6.conf.$tap.disable_ipv6=1" >/dev/null
fi

rm -f "$eth_trace"
TRIBE_INTERACTIVE_CYCLES="${TRIBE_INTERACTIVE_SSH_CYCLES:-0}" \
  TRIBE_INTERACTIVE_SD_IMAGE="$rootfs" \
  TRIBE_INTERACTIVE_ETH_TRACE="$eth_trace" \
  "$root/tests/tribe/tribe_interactive.sh" "${multicore[@]}" \
  "${simulator_backend[@]}" --tap-socket "$tap_socket" \
  <"$input" >"$log" 2>&1 &
simulator_pid=$!

wait_for_log() {
  local marker="$1"
  local deadline=$((SECONDS + wall_timeout))
  while ((SECONDS < deadline)); do
    if rg -q "$marker" "$log" 2>/dev/null; then
      return 0
    fi
    if ! kill -0 "$simulator_pid" 2>/dev/null; then
      sed -n '1,420p' "$log" >&2
      echo "interactive Tribe exited before marker: $marker" >&2
      return 1
    fi
    if [[ -n "${ssh_pid:-}" ]] && ! kill -0 "$ssh_pid" 2>/dev/null; then
      sed -n '1,420p' "$log" >&2
      sed -n '1,240p' "$ssh_log" >&2
      echo "SSH client exited before marker: $marker" >&2
      return 1
    fi
    sleep 0.1
  done
  sed -n '1,420p' "$log" >&2
  echo "timed out waiting for marker: $marker" >&2
  return 1
}

wait_for_log_count() {
  local marker="$1"
  local expected="$2"
  local deadline=$((SECONDS + wall_timeout))
  local count
  while ((SECONDS < deadline)); do
    count="$(rg -c "$marker" "$log" 2>/dev/null || true)"
    if (( ${count:-0} >= expected )); then
      return 0
    fi
    if ! kill -0 "$simulator_pid" 2>/dev/null; then
      sed -n '1,520p' "$log" >&2
      echo "interactive Tribe exited before $expected occurrences of: $marker" >&2
      return 1
    fi
    if [[ -n "${ssh_pid:-}" ]] && ! kill -0 "$ssh_pid" 2>/dev/null; then
      sed -n '1,520p' "$log" >&2
      sed -n '1,240p' "$ssh_log" >&2
      echo "SSH client exited before $expected occurrences of: $marker" >&2
      return 1
    fi
    sleep 0.1
  done
  sed -n '1,520p' "$log" >&2
  echo "timed out waiting for $expected occurrences of: $marker" >&2
  return 1
}

wait_for_ssh_log() {
  local marker="$1"
  local deadline=$((SECONDS + wall_timeout))
  while ((SECONDS < deadline)); do
    if rg -q "$marker" "$ssh_log" 2>/dev/null; then
      return 0
    fi
    if ! kill -0 "$simulator_pid" 2>/dev/null ||
       ! kill -0 "$ssh_pid" 2>/dev/null; then
      sed -n '1,620p' "$log" >&2
      sed -n '1,320p' "$ssh_log" >&2
      echo "SSH session exited before client-output marker: $marker" >&2
      return 1
    fi
    sleep 0.1
  done
  sed -n '1,620p' "$log" >&2
  sed -n '1,320p' "$ssh_log" >&2
  echo "timed out waiting for SSH client-output marker: $marker" >&2
  return 1
}

start_ssh_client() {
  local remote_marker="$1"
  local request_pty="${2:-0}"
  local interactive_shell="${3:-0}"
  local -a client_arguments
  local -a client_command
  local client_shell_command
  local remote_command
  local ssh_input="$runtime/ssh.input"
  # tribe_interactive.sh installs a permanent fixed-MAC neighbor entry.  Do
  # not replace it with slow ARP resolution between serialized sessions.
  client_arguments=(-i "$dropbear_identity" -c none -y -y -p "$guest_port")
  remote_command="echo $remote_marker"
  if [[ "$request_pty" == 1 ]]; then
    client_arguments+=(-t)
    # Keep this command inside the already-forked shell. Invoking the external
    # BusyBox `tty` applet would require another process level and would turn
    # this PTY transport regression into a nested-fork-capacity test. Dropbear
    # supplies SSH_TTY only after allocating the remote PTY.
    remote_command="printf '%s\\n' \"\$SSH_TTY\"; echo $remote_marker"
  else
    client_arguments+=(-T)
  fi
  client_command=("$root/build/tests/busybox/dropbear-host/dbclient"
                  "${client_arguments[@]}" "root@$guest_address")
  if [[ "$interactive_shell" != 1 ]]; then
    client_command+=("$remote_command")
  fi
  if [[ "$request_pty" == 1 ]]; then
    # The regression harness itself has no controlling terminal. dbclient -t
    # must nevertheless see a real local PTY so it can read termios modes and
    # send the same PTY request as the interactive command printed by
    # tribe_interactive.sh.
    printf -v client_shell_command '%q ' "${client_command[@]}"
    if [[ "$interactive_shell" == 1 ]]; then
      # Keep the input FIFO open so the test can type only after each remote
      # prompt. Sending the whole transcript up front would leave post-ls
      # input buffered in the slave and fail to exercise its blocked wakeup.
      rm -f "$ssh_input"
      mkfifo "$ssh_input"
      exec {ssh_input_fd}<>"$ssh_input"
      script -qefc "$client_shell_command" /dev/null \
        <"$ssh_input" >"$ssh_log" 2>&1 &
    else
      script -qefc "$client_shell_command" /dev/null >"$ssh_log" 2>&1 &
    fi
    ssh_pid=$!
    ssh_pause_target=""
    for _ in $(seq 1 100); do
      ssh_pause_target="$(pgrep -P "$ssh_pid" 2>/dev/null | head -1 || true)"
      [[ -n "$ssh_pause_target" ]] && break
      sleep 0.01
    done
    if [[ -z "$ssh_pause_target" ]]; then
      echo "failed to discover dbclient child of PTY wrapper $ssh_pid" >&2
      return 1
    fi
  else
    "${client_command[@]}" >"$ssh_log" 2>&1 &
    ssh_pid=$!
    ssh_pause_target="$ssh_pid"
  fi
}

finish_ssh_client() {
  local remote_marker="$1"
  local accept_count="$2"
  local client_status
  local marker_seen=0
  if [[ -z "$remote_marker" ]]; then
    marker_seen=1
  fi
  wait_for_log_count 'MIKOS:TCP_ACCEPT fd=' "$accept_count"
  local deadline=$((SECONDS + wall_timeout))
  while ((SECONDS < deadline)); do
    # A remote PTY applies ONLCR, and util-linux script's local PTY can apply
    # it again. Accept the resulting LF, CRLF, or CRCRLF line ending while
    # still requiring the marker to occupy the complete logical line.
    if [[ -n "$remote_marker" ]] &&
       rg -q "^${remote_marker}\r*$" "$ssh_log" 2>/dev/null; then
      marker_seen=1
    fi
    # A nested command must restore the login shell's real PPID. A zero PPID
    # makes the PTY scheduler reject every subsequent wakeup, so fail at the
    # first corrupt park instead of consuming the full native-C++ timeout.
    if rg -q '^MIKOS:PTY_CHILD_PARK child=[1-9][0-9]* parent=0([[:space:]]|$)' \
        "$log" 2>/dev/null; then
      sed -n '1,520p' "$log" >&2
      sed -n '1,240p' "$ssh_log" >&2
      echo "FAIL: SSH session $accept_count restored a PTY child with PPID 0" >&2
      return 1
    fi
    if ! kill -0 "$ssh_pid" 2>/dev/null; then
      if wait "$ssh_pid"; then
        client_status=0
      else
        client_status=$?
      fi
      ssh_pid=""
      ssh_pause_target=""
      if [[ -n "${ssh_input_fd:-}" ]]; then
        exec {ssh_input_fd}>&-
        ssh_input_fd=""
      fi
      if ((marker_seen == 1 && client_status == 0)); then
        return 0
      fi
      sed -n '1,520p' "$log" >&2
      sed -n '1,240p' "$ssh_log" >&2
      echo "FAIL: SSH session $accept_count ended without successful marker/close; marker=$marker_seen dbclient=$client_status" >&2
      return 1
    fi
    sleep 0.1
  done
  sed -n '1,520p' "$log" >&2
  sed -n '1,240p' "$ssh_log" >&2
  echo "FAIL: SSH session $accept_count timed out; marker=$marker_seen and dbclient is still running" >&2
  return 1
}

exercise_interactive_shell() {
  local park_count
  local resume_count
  local child_exit_count
  local relay_count

  # Wait until dbclient has switched its local pseudo-terminal to raw mode and
  # the remote shell is blocked at its first prompt. VINTR sent earlier is
  # consumed by the local `script` PTY and merely echoed as ^C.
  wait_for_log 'MIKOS:PTY_CHILD_PARK child=[1-9][0-9]* parent=[1-9][0-9]*'
  park_count="$(rg -c 'MIKOS:PTY_CHILD_PARK child=' "$log" 2>/dev/null || true)"
  resume_count="$(rg -c 'MIKOS:PTY_CHILD_RESUME child=' "$log" 2>/dev/null || true)"
  child_exit_count="$(rg -c 'MIKOS:CHILD_EXIT pid=[1-9][0-9]* status=0' \
    "$log" 2>/dev/null || true)"

  # BusyBox's interactive line editor may put the slave in raw mode. In that
  # state VINTR is correctly delivered as a byte and handled by ash itself;
  # in cooked mode the PTY model generates SIGINT. Either way, Ctrl-C must
  # wake the parked shell and return it to a fresh prompt wait.
  printf '\003' >&"$ssh_input_fd"
  wait_for_log_count 'MIKOS:PTY_CHILD_RESUME child=' "$((resume_count + 1))"
  wait_for_log_count 'MIKOS:PTY_CHILD_PARK child=' "$((park_count + 1))"

  # Batch the foreground-app command behind ls. util-linux script can split a
  # command typed after a fresh prompt into one-byte SSH channel packets; each
  # byte would force a multi-minute Dropbear/BusyBox image swap in native C++.
  # BusyBox consumes this batch one line at a time, so top still starts only
  # after ls exits and exercises the identical nested PTY-blocking state.
  relay_count="$(rg -c 'MIKOS:PTY_CHILD_RELAY child=' "$log" 2>/dev/null || true)"
  printf '%s\n' \
    'printf '\''%s\n'\'' "$SSH_TTY"' \
    'ls /' \
    'top' >&"$ssh_input_fd"
  wait_for_log 'MIKOS:NESTED_CLONE_STACK depth=3'
  wait_for_log 'MIKOS:EXEC pid=[1-9][0-9]* path=/bin/(ls|sh).*argv0=ls'
  wait_for_log_count 'MIKOS:CHILD_EXIT pid=[1-9][0-9]* status=0' \
    "$((child_exit_count + 1))"
  wait_for_log 'MIKOS:EXEC pid=[1-9][0-9]* path=/bin/(top|sh).*argv0=top'
  wait_for_log_count 'MIKOS:PTY_CHILD_RELAY child=' "$((relay_count + 1))"
  wait_for_log_count 'MIKOS:PTY_CHILD_PARK child=' "$((park_count + 2))"
  if rg -q '^MIKOS:PTY_CHILD_PARK child=[1-9][0-9]* parent=0([[:space:]]|$)' \
      "$log"; then
    echo "FAIL: nested command restored the SSH shell with PPID 0" >&2
    return 1
  fi

  # A full-screen foreground app blocks one process level below the shell.
  # Its PTY wait must relay past that waiting shell to Dropbear, then rebuild
  # the same ancestry when input arrives. Exercise two top wakeups, its clean
  # exit, and the restored shell's next prompt so a one-way handoff cannot
  # pass.
  park_count="$(rg -c 'MIKOS:PTY_CHILD_PARK child=' "$log" 2>/dev/null || true)"
  resume_count="$(rg -c 'MIKOS:PTY_CHILD_RESUME child=' "$log" 2>/dev/null || true)"
  child_exit_count="$(rg -c 'MIKOS:CHILD_EXIT pid=[1-9][0-9]* status=0' \
    "$log" 2>/dev/null || true)"
  relay_count="$(rg -c 'MIKOS:PTY_CHILD_RELAY child=' "$log" 2>/dev/null || true)"
  printf '1' >&"$ssh_input_fd"
  wait_for_log_count 'MIKOS:PTY_CHILD_RESUME child=' "$((resume_count + 1))"
  wait_for_log_count 'MIKOS:PTY_CHILD_RELAY child=' "$((relay_count + 1))"
  wait_for_log_count 'MIKOS:PTY_CHILD_PARK child=' "$((park_count + 1))"
  wait_for_ssh_log 'Mem:'
  printf 'q' >&"$ssh_input_fd"
  wait_for_log_count 'MIKOS:PTY_CHILD_RESUME child=' "$((resume_count + 2))"
  wait_for_log_count 'MIKOS:CHILD_EXIT pid=[1-9][0-9]* status=0' \
    "$((child_exit_count + 1))"
  wait_for_log_count 'MIKOS:PTY_CHILD_PARK child=' "$((park_count + 2))"

  # EOF at an empty interactive prompt is a one-byte clean logout. A longer
  # `exit\n` transcript can be split by dbclient into character-sized SSH
  # channel packets, forcing an expensive address-space handoff per byte.
  printf '\004' >&"$ssh_input_fd"
}

wait_for_log "MIKOS:TCP_LISTEN $guest_port"
if [[ "$early_client" == 1 ]]; then
  start_ssh_client MIKOS_SSH_AUTH_OK_1 "$pty_only" "$pty_only"
else
  wait_for_log 'MIKOS_SSH_STARTING 192\.168\.76\.2:22 pid='
  wait_for_log '/ # '
  if [[ "$late_diagnostics" == 1 ]]; then
    printf 'ps\n' >&3
    wait_for_log 'dropbear([[:space:]]|$)'
    printf 'ifconfig -a\n' >&3
    wait_for_log 'inet addr:192\.168\.76\.2'
    printf 'netstat -ln\n' >&3
    wait_for_log '^tcp[[:space:]]+0[[:space:]]+0[[:space:]]+0\.0\.0\.0:22[[:space:]]+0\.0\.0\.0:\*[[:space:]]+LISTEN'
  fi
  start_ssh_client MIKOS_SSH_AUTH_OK_1 "$pty_only" "$pty_only"
fi

wait_for_log 'MIKOS:TCP_ACCEPT fd='
wait_for_log 'MIKOS:TCP_WRITE 26'
wait_for_log 'MIKOS:TCP_READ '
if [[ "$uart_preempt_probe" == 1 ]]; then
  # Stop the peer before the KEX reply, then leave UART input pending. This
  # forces the nested SSH child into BACKGROUND_CONNECTION_HOLD with no network
  # data available. It must ignore UART rather than park and incorrectly resume
  # the listener from the stacked ancestry.
  wait_for_log 'MIKOS_DROPBEAR_X25519_COMBINE_DONE'
  connection_pid="$(sed -n \
    's/^\[\([0-9][0-9]*\)\].*Child connection from.*/\1/p' "$log" | tail -1)"
  if [[ -z "$connection_pid" ]]; then
    echo "FAIL: could not discover guest SSH connection PID" >&2
    exit 1
  fi
  kill -STOP "$ssh_pause_target"
  printf '\n' >&3
  wait_for_log "MIKOS:BACKGROUND_CONNECTION_HOLD $connection_pid"
  if rg -q "MIKOS:BACKGROUND_PARK $connection_pid|MIKOS:BACKGROUND_PARK_NO_PARENT" "$log"; then
    echo "FAIL: nested SSH child yielded to UART while holding a live connection" >&2
    exit 1
  fi
  kill -CONT "$ssh_pause_target"
  # util-linux script may stop itself when its PTY child reports SIGSTOP.
  kill -CONT "$ssh_pid" 2>/dev/null || true
fi
if [[ "$pty_only" == 1 ]]; then
  exercise_interactive_shell
  finish_ssh_client '' 1
else
  finish_ssh_client MIKOS_SSH_AUTH_OK_1 1
fi
if [[ "$pty_only" == 1 ]]; then
  if ! rg -q '^/dev/pts/[0-3]\r*$' "$ssh_log"; then
    sed -n '1,240p' "$ssh_log" >&2
    echo "FAIL: SSH session did not allocate a working remote PTY" >&2
    exit 1
  fi
  echo "PASS: Dropbear completed an interactive PTY none-mode public-key SSH session on $guest_address:$guest_port"
  exit 0
fi

# The same daemon must retain its listener after the first session child is
# reaped. The second connection is deliberately an interactive login shell,
# not a remote command with a PTY. It must write a prompt, yield from a blocked
# slave read so Dropbear can relay the prompt/input, resume, execute typed
# commands, close, and leave the listener reusable.
start_ssh_client MIKOS_SSH_AUTH_OK_2 1 1
exercise_interactive_shell
finish_ssh_client '' 2
if ! rg -q '^/dev/pts/[0-3]\r*$' "$ssh_log"; then
  sed -n '1,240p' "$ssh_log" >&2
  echo "FAIL: second SSH session did not allocate a working remote PTY" >&2
  exit 1
fi

echo "PASS: persistent Dropbear completed command and interactive PTY none-mode public-key SSH sessions on $guest_address:$guest_port"
