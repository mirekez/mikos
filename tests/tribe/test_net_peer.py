"""Validate a final echo queued behind ARP when the simulator socket closes."""

import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return (~total) & 65535


def packet(frame):
    return b"\2" + struct.pack("!H", len(frame)) + frame


def check(binary, corrupt=False):
    with tempfile.TemporaryDirectory(prefix="mikos-peer-check-") as directory:
        server, client = directory + "/server", directory + "/client"
        peer = subprocess.Popen(
            [binary, server], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
        try:
            for _ in range(200):
                if Path(server).exists():
                    break
                time.sleep(0.01)
            with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as simulator:
                simulator.settimeout(5)
                simulator.bind(client)
                simulator.sendto(b"\1", server)
                arp = bytearray(simulator.recv(4096)[3:])
                guest = bytes.fromhex("020000000002")
                host = bytes(arp[6:12])
                host_ip, guest_ip = bytes(arp[28:32]), bytes(arp[38:42])
                arp[:6], arp[6:12], arp[20:22] = host, guest, b"\0\2"
                arp[22:28], arp[28:32] = guest, guest_ip
                arp[32:38], arp[38:42] = host, host_ip
                simulator.sendto(packet(arp), server)
                echo = bytearray(simulator.recv(4096)[3:])

                # Hold the peer so both replies are queued before socket removal.
                os.kill(peer.pid, signal.SIGSTOP)
                os.waitpid(peer.pid, os.WUNTRACED)
                echo[:6], echo[6:12] = host, guest
                echo[26:30], echo[30:34] = guest_ip, host_ip
                echo[22] = 64
                echo[24:26] = b"\0\0"
                echo[24:26] = struct.pack("!H", checksum(bytes(echo[14:34])))
                echo[34] = 0
                echo[36:38] = b"\0\0"
                echo[36:38] = struct.pack("!H", checksum(bytes(echo[34:])))
                simulator.sendto(packet(arp), server)
                if corrupt:
                    echo[-1] ^= 1
                simulator.sendto(packet(echo), server)
            os.unlink(client)
            os.kill(peer.pid, signal.SIGCONT)
            output, errors = peer.communicate(timeout=5)
            expected = 1 if corrupt else 0
            if peer.returncode != expected:
                raise AssertionError(
                    f"peer exit {peer.returncode}, expected {expected}: {output}{errors}"
                )
        finally:
            if peer.poll() is None:
                peer.kill()
                peer.wait()


if __name__ == "__main__":
    check(sys.argv[1])
    check(sys.argv[1], corrupt=True)
    print("PASS: Tribe peer drains queued replies after shutdown and rejects corrupt payloads")
