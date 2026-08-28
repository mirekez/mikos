#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
simulator_name="tribe64"
kernel="$root/build/mikos-tribe-interactive-rv32.elf"
kernel_target="tribe-interactive-kernel"
cycles="${TRIBE_INTERACTIVE_CYCLES:-0}"
tap_socket="${TRIBE_ETH_TAP_SOCKET:-/tmp/tribe-ethgig.sock}"
tap_socket_was_explicit="${TRIBE_ETH_TAP_SOCKET+x}"
tap_name="${TRIBE_INTERACTIVE_TAP:-tap-tribe}"
host_address="${TRIBE_INTERACTIVE_HOST_ADDRESS:-192.168.76.1}"
guest_address="${TRIBE_INTERACTIVE_GUEST_ADDRESS:-192.168.76.2}"
guest_mac="${TRIBE_INTERACTIVE_GUEST_MAC:-02:00:00:00:00:02}"
use_verilator="${TRIBE_INTERACTIVE_USE_VERILATOR:-0}"
bridge_tag="mikos-reliable-v5"
bridge_pcap="${TRIBE_INTERACTIVE_PCAP:-/tmp/mikos-tribe-${UID}.pcap}"
eth_trace="${TRIBE_INTERACTIVE_ETH_TRACE:-/tmp/mikos-tribe-${UID}.eth.log}"
verilator_cores=1
test_mode=""

usage() {
  echo "usage: $0 [--multicore] [--verilator] [--tap-socket PATH] [--test ping|tcp|process|ssh]" >&2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --multicore)
      simulator_name="tribe64_multicore"
      kernel="$root/build/mikos-tribe-interactive-multicore-rv32.elf"
      kernel_target="tribe-interactive-multicore-kernel"
      verilator_cores=4
      ;;
    --verilator)
      use_verilator=1
      ;;
    --test)
      if [[ $# -lt 2 ]]; then
        usage
        echo "--test requires ping, tcp, process, or ssh" >&2
        exit 2
      fi
      test_mode="$2"
      shift
      ;;
    --tap-socket)
      if [[ $# -lt 2 ]]; then
        usage
        echo "--tap-socket requires a path" >&2
        exit 2
      fi
      tap_socket="$2"
      tap_socket_was_explicit=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
  shift
done

if [[ "$use_verilator" != 0 && "$use_verilator" != 1 ]]; then
  echo "TRIBE_INTERACTIVE_USE_VERILATOR must be 0 or 1" >&2
  exit 2
fi

if [[ -n "$test_mode" ]]; then
  case "$test_mode" in
    ping|tcp|process|ssh)
      ;;
    *)
      usage
      echo "unknown interactive test: $test_mode" >&2
      exit 2
      ;;
  esac
  test_arguments=()
  if [[ "$simulator_name" == "tribe64_multicore" ]]; then
    test_arguments+=(--multicore)
  fi
  exec env TRIBE_INTERACTIVE_USE_VERILATOR="$use_verilator" \
    "$root/tests/tribe/run_interactive_${test_mode}.sh" \
    "${test_arguments[@]}"
fi

simulator="$root/build/tests/tribe/cpphdl-build/$simulator_name/$simulator_name"
rootfs="${TRIBE_INTERACTIVE_SD_IMAGE:-$root/build/tests/busybox/rootfs.ext4}"
client_key="$root/build/tests/busybox/dropbear-host/mikos_ssh_key.dropbear"
maintained_bridge="$root/build/tests/qemu/ethgig_tap"

if [[ ! -x "$simulator" ]] ||
   [[ "$root/tests/tribe/prepare_cpphdl.sh" -nt "$simulator" ]] ||
   find "$root/tests/tribe/patches" -type f -newer "$simulator" -print -quit |
     rg -q .; then
  echo "Tribe simulator is missing; preparing cpphdl..." >&2
  prepare_arguments=()
  if [[ "$simulator_name" == "tribe64_multicore" ]]; then
    prepare_arguments+=(--multicore)
  fi
  "$root/tests/tribe/prepare_cpphdl.sh" "${prepare_arguments[@]}"
fi

make -C "$root" "$kernel_target" dropbear-client "$maintained_bridge"

bridge_pid=""
bridge_command=""
while read -r candidate_pid candidate_command; do
  candidate_executable="${candidate_command%% *}"
  # ps also reports the sudo parent processes.  Only the bridge executable
  # owns the socket and TAP, and killing a sudo wrapper can orphan that bridge.
  [[ "${candidate_executable##*/}" == "ethgig_tap" ]] || continue
  case " $candidate_command " in
    *" --socket $tap_socket "*)
      bridge_pid="$candidate_pid"
      bridge_command="$candidate_command"
      break
      ;;
  esac
done < <(ps -eo pid=,args= | rg '[e]thgig_tap' || true)

start_maintained_bridge=0
if [[ -z "$tap_socket_was_explicit" ]]; then
  if [[ -n "$bridge_pid" &&
        " $bridge_command " != *" --instance-tag $bridge_tag "* ]]; then
    if [[ ! -t 0 || ! -x "$(command -v sudo 2>/dev/null || true)" ]]; then
      echo "Incompatible persistent Tribe bridge owns $tap_socket:" >&2
      echo "  pid $bridge_pid: $bridge_command" >&2
      echo "Run interactively once so this script can replace it with the maintained bridge." >&2
      exit 1
    fi
    echo "Replacing incompatible persistent Tribe TAP bridge (pid $bridge_pid)..." >&2
    sudo kill "$bridge_pid"
    for _ in $(seq 1 100); do
      [[ -d "/proc/$bridge_pid" ]] || break
      sleep 0.02
    done
    if [[ -d "/proc/$bridge_pid" ]]; then
      echo "Old Tribe TAP bridge did not stop: pid $bridge_pid" >&2
      exit 1
    fi
    start_maintained_bridge=1
  elif [[ ! -S "$tap_socket" ]]; then
    start_maintained_bridge=1
  elif [[ -z "$bridge_pid" ]]; then
    echo "Bridge socket exists but its owner cannot be identified: $tap_socket" >&2
    echo "Remove the stale socket or set TRIBE_ETH_TAP_SOCKET for an externally managed bridge." >&2
    exit 1
  fi

  if [[ "$start_maintained_bridge" == 1 ]]; then
    if [[ ! -t 0 || ! -x "$(command -v sudo 2>/dev/null || true)" ]]; then
      echo "Starting $tap_name requires an interactive sudo once." >&2
      exit 1
    fi
    bridge_log="/tmp/mikos-tribe-bridge-${UID}.log"
    : >"$bridge_log"
    echo "Starting maintained persistent MikOS TAP bridge..." >&2
    sudo -b env MIKOS_ETH_TAP_PCAP="$bridge_pcap" \
      "$maintained_bridge" --tap "$tap_name" \
      --address "$host_address/24" --socket "$tap_socket" \
      --instance-tag "$bridge_tag" >"$bridge_log" 2>&1
    for _ in $(seq 1 200); do
      [[ -S "$tap_socket" ]] && ip link show dev "$tap_name" >/dev/null 2>&1 && break
      sleep 0.02
    done
    if [[ ! -S "$tap_socket" ]] ||
       ! ip link show dev "$tap_name" >/dev/null 2>&1; then
      cat "$bridge_log" >&2
      echo "Maintained Tribe TAP bridge did not become ready." >&2
      exit 1
    fi
    sudo sysctl -q -w "net.ipv6.conf.$tap_name.disable_ipv6=1" >/dev/null || true
  fi
fi

if [[ ! -S "$tap_socket" ]]; then
  echo "Tribe TAP bridge socket is missing: $tap_socket" >&2
  exit 1
fi
if ! "$maintained_bridge" --probe-socket "$tap_socket"; then
  echo "The current user cannot connect to the Tribe TAP bridge socket:" >&2
  ls -l "$tap_socket" >&2 || true
  echo "Refusing to start the CPU simulator with an unusable network link." >&2
  exit 1
fi
if ! ip link show dev "$tap_name" >/dev/null 2>&1; then
  echo "Host TAP interface is missing: $tap_name" >&2
  exit 1
fi
if ! ip -4 -o address show dev "$tap_name" | \
     rg -q "[[:space:]]${host_address}/24([[:space:]]|$)"; then
  echo "Host TAP $tap_name does not own $host_address/24." >&2
  exit 1
fi

# ARP resolution can take longer than Linux's neighbor-probe timeout in the
# cycle-level model.  A failed flush is especially misleading because ip(8)
# requires CAP_NET_ADMIN and the old command hid that failure.  Both endpoint
# MACs are fixed in this test profile, so install a permanent host mapping.
neighbor_pattern="^${guest_address}([[:space:]]+dev[[:space:]]+${tap_name})?[[:space:]]+lladdr[[:space:]]+${guest_mac}[[:space:]]+PERMANENT$"
if ! ip neigh show to "$guest_address" dev "$tap_name" | rg -qi "$neighbor_pattern"; then
  if ! ip neigh replace "$guest_address" lladdr "$guest_mac" \
       nud permanent dev "$tap_name" 2>/dev/null; then
    if [[ -t 0 ]] && command -v sudo >/dev/null 2>&1; then
      echo "Configuring permanent MikOS neighbor entry (sudo is required once for this TAP)..." >&2
      sudo ip neigh replace "$guest_address" lladdr "$guest_mac" \
        nud permanent dev "$tap_name"
    else
      echo "warning: cannot configure the permanent MikOS neighbor entry noninteractively." >&2
      echo "For reliable manual SSH, run once:" >&2
      echo "  sudo ip neigh replace $guest_address lladdr $guest_mac nud permanent dev $tap_name" >&2
      echo "Continuing with ARP fallback for this automated run." >&2
    fi
  fi
fi

if [[ ! -t 0 ]]; then
  echo "warning: stdin is not a terminal; UART input will follow stdin" >&2
fi

echo "Starting MikOS interactive BusyBox shell." >&2
echo "Type 'exit' to stop MikOS; Ctrl+C stops the simulator; Ctrl+Z suspends it." >&2
echo "Host link: $tap_name $host_address/24; guest: $guest_address/24." >&2
echo "Packet capture: $bridge_pcap" >&2
echo "Simulator Ethernet trace: $eth_trace" >&2
echo "SSH after MIKOS_SSH_STARTING (sessions are serialized):" >&2
echo "  $root/build/tests/busybox/dropbear-host/dbclient -i $client_key -c none -y -y root@$guest_address" >&2
echo "Do not wrap dbclient in 'timeout 1000': native C++ authentication can take about 25 minutes." >&2
echo "The -i Dropbear-format identity above is required; dbclient's default identity will not authenticate." >&2
echo "After exiting dbclient, wait for Dropbear to return to its listen loop before reconnecting." >&2

simulator_arguments=(--noveril)
if [[ "$use_verilator" == 1 ]]; then
  verilator_simulator="$(dirname "$simulator")/TribeTest_${verilator_cores}/obj_dir/VTribeTest"
  if [[ ! -x "$verilator_simulator" || "$simulator" -nt "$verilator_simulator" ]]; then
    echo "Building the optional Verilator model for interactive execution..." >&2
    "$simulator" 1
  fi
  if [[ ! -x "$verilator_simulator" ]]; then
    echo "Verilator model was not produced: $verilator_simulator" >&2
    exit 1
  fi
  simulator="$verilator_simulator"
  simulator_arguments=()
fi

exec env TRIBE_TRACE_ETH=1 TRIBE_TRACE_ETH_FILE="$eth_trace" \
  "$simulator" "${simulator_arguments[@]}" \
  --program "$kernel" --elf \
  --cycles "$cycles" \
  --start-mem-addr 0x80000000 \
  --ram-size 8388608 \
  --boot-priv m \
  --sd-image "$rootfs" \
  --uart-stdin \
  --eth-tap-socket "$tap_socket" \
  --expected-output-contains 'MIKOS:EXIT 0'
