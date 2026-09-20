#!/usr/bin/env python3
"""Exercise production TCP against Linux through an already-configured TAP.

Only run while that bridge is not serving a CPU simulator. No interface or
neighbor configuration is changed by this test.
"""
import argparse
import pathlib
import socket
import subprocess
import tempfile
import threading
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bridge', default='/tmp/tribe-ethgig.sock')
    parser.add_argument('--binary', type=pathlib.Path, required=True)
    args = parser.parse_args()
    if not pathlib.Path(args.bridge).is_socket():
        parser.error('an existing Tribe TAP bridge is required (see tests/tribe/README.md)')
    with tempfile.TemporaryDirectory(prefix='mikos-linux-tcp-') as runtime:
        server = subprocess.Popen([str(args.binary), args.bridge, runtime + '/peer.sock'],
                                  stdout=subprocess.PIPE, text=True)
        try:
            assert server.stdout.readline().strip() == 'READY', 'server did not initialize'
            for session, length in enumerate((64_000, 256_000, 64_000)):
                payload = bytes((i * 37 + session * 13) % 251 for i in range(length))
                with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as client:
                    client.settimeout(15)
                    client.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
                    client.connect(('192.168.76.2', 2223))
                    failures = []
                    def send():
                        try:
                            client.sendall(payload)
                            client.shutdown(socket.SHUT_WR)
                        except Exception as error:
                            failures.append(error)
                    sender = threading.Thread(target=send)
                    sender.start()
                    # The small receive buffer and delayed reader force Linux
                    # to close/reopen its advertised window on the larger stream.
                    if session == 1:
                        time.sleep(1.5)
                    result = bytearray()
                    try:
                        while block := client.recv(137):
                            result.extend(block)
                    finally:
                        sender.join(timeout=16)
                    assert not sender.is_alive(), 'sender did not finish'
                    assert not failures, failures
                    assert result == payload, f'byte mismatch in session {session}'
                    print(f'PASS: Linux peer session {session + 1}: {length} bytes, half-close, EOF')
            assert server.wait(timeout=5) == 0, 'server failed'
            print(server.stdout.read(), end='')
        finally:
            if server.poll() is None:
                server.terminate()
                server.wait(timeout=5)


if __name__ == '__main__':
    main()
