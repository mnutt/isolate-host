#!/usr/bin/env python3
"""Exercise the production inherited-socket mode and controller-disconnect lifetime."""
import socket
import subprocess
import sys

host, client = sys.argv[1:]
server_socket, client_socket = socket.socketpair()
process = subprocess.Popen([host, '--control-fd', str(server_socket.fileno())],
                           pass_fds=(server_socket.fileno(),))
server_socket.close()
try:
    worker = subprocess.Popen([client, '--control-fd', str(client_socket.fileno())],
                              pass_fds=(client_socket.fileno(),))
    client_socket.close()
    try:
        if worker.wait(timeout=60) != 0:
            raise RuntimeError('inherited-FD control tests failed')
    finally:
        if worker.poll() is None:
            worker.kill()
            worker.wait()
    if process.wait(timeout=10) != 0:
        raise RuntimeError('host failed after controller disconnected')
finally:
    client_socket.close()
    if process.poll() is None:
        process.kill()
        process.wait()
