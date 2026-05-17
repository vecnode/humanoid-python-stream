"""
Shared protocol constants and helpers for the audio worker socket bridge.

The CLoSD process (Python 3.8 / closd env) is the CLIENT.
The audio worker (Python 3.10+ / .venv)    is the SERVER.

Messages are newline-delimited JSON over a plain TCP socket on localhost.

Client → Server:
  {"cmd": "ping"}
  {"cmd": "synthesize", "text": "...", "speaker": "Carter"}
  {"cmd": "shutdown"}

Server → Client:
  {"status": "pong", "ready": true}
  {"status": "ok"}
  {"status": "error", "msg": "..."}
"""

import json
import socket

WORKER_HOST = "127.0.0.1"
WORKER_PORT = 45679
CONNECT_TIMEOUT = 2.0   # seconds — how long CLoSD waits to reach the worker
RECV_BUFSIZE = 4096


def send_msg(sock: socket.socket, payload: dict) -> None:
    """Send one JSON message (newline-terminated)."""
    data = json.dumps(payload) + "\n"
    sock.sendall(data.encode("utf-8"))


def recv_msg(sock_file) -> dict:
    """Read one newline-terminated JSON message from a file-like socket."""
    line = sock_file.readline()
    if not line:
        raise ConnectionResetError("socket closed")
    return json.loads(line.strip())
