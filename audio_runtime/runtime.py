"""CLoSD-side audio runtime client (Python 3.8 / closd env).

Connects to the audio worker process (running in .venv / Python 3.10+)
via a local TCP socket. All model loading happens in the worker; this module
only sends text prompts and receives status acknowledgements.

The interface is intentionally identical to the previous direct-load design
so callers in run.py do not need to change.
"""

import socket
import threading
from dataclasses import dataclass, field
from typing import Any, Dict, Optional

from .config import AudioRuntimeConfig
from .protocol import CONNECT_TIMEOUT, send_msg, recv_msg


@dataclass
class AudioRuntime:
    config: AudioRuntimeConfig
    ready: bool
    _sock: Any = field(default=None, repr=False)
    _sock_file: Any = field(default=None, repr=False)
    _lock: threading.Lock = field(default_factory=threading.Lock, repr=False)

    def is_ready(self) -> bool:
        return self.ready

    def submit_conditioning(
        self,
        pose_tensor: Any,
        phase_tensor: Any,
        meta: Optional[Dict[str, Any]] = None,
    ) -> bool:
        """Send a text prompt to the audio worker.

        The text is taken from meta["text"] when present.  Pose / phase
        tensors are accepted for API compatibility but not yet forwarded
        (Phase 2 will stream conditioning features).
        """
        if not self.ready or self._sock is None:
            return False
        text = (meta or {}).get("text", "").strip()
        if not text:
            return False
        try:
            with self._lock:
                send_msg(self._sock, {"cmd": "synthesize", "text": text})
            return True
        except Exception as ex:
            print("[audio] send failed; marking not-ready: %s" % ex)
            self.ready = False
            return False

    def shutdown(self) -> None:
        self.ready = False
        try:
            if self._sock:
                send_msg(self._sock, {"cmd": "shutdown"})
        except Exception:
            pass
        try:
            if self._sock:
                self._sock.close()
        except Exception:
            pass


def initialize_audio_runtime(cfg_like: Any) -> Optional[AudioRuntime]:
    cfg = AudioRuntimeConfig.from_cfg(cfg_like)
    if not cfg.enabled:
        print("[audio] disabled")
        return None

    print("[audio] enabled=True worker=%s:%d" % (cfg.worker_host, cfg.worker_port))
    print("[audio] connecting to worker...")

    try:
        sock = socket.create_connection(
            (cfg.worker_host, cfg.worker_port), timeout=CONNECT_TIMEOUT
        )
        sock.settimeout(None)  # blocking after connect
        sock_file = sock.makefile("r", encoding="utf-8")

        # ping to confirm worker is alive and model is loaded
        send_msg(sock, {"cmd": "ping"})
        reply = recv_msg(sock_file)
        if reply.get("status") != "pong":
            raise RuntimeError("unexpected ping reply: %s" % reply)

        runtime = AudioRuntime(
            config=cfg,
            ready=bool(reply.get("ready", True)),
            _sock=sock,
            _sock_file=sock_file,
        )
        print("[audio] connected to worker; ready=%s" % runtime.ready)
        return runtime

    except Exception as ex:
        print("[audio] could not connect to worker; continuing without audio: %s" % ex)
        print("[audio] start the worker with: .venv/bin/python -m audio_runtime.worker")
        return None
