"""State bridge utilities for external realtime viewers.

This module keeps dependencies minimal and can be imported from task code
without changing planner or policy logic.
"""

from __future__ import annotations

import json
import socket
import time
from dataclasses import dataclass
from typing import Any, Dict, Iterable, Optional


@dataclass
class BridgeConfig:
    host: str = "127.0.0.1"
    port: int = 45678
    enabled: bool = False
    max_bytes: int = 60000


class PilotLightStatePublisher:
    """Publishes frame packets over UDP with best-effort non-blocking sends."""

    def __init__(self, cfg: BridgeConfig) -> None:
        self.cfg = cfg
        self._sock: Optional[socket.socket] = None
        self._seq = 0

        if self.cfg.enabled:
            self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self._sock.setblocking(False)

    def close(self) -> None:
        if self._sock is not None:
            self._sock.close()
            self._sock = None

    def publish(self, payload: Dict[str, Any]) -> bool:
        if not self.cfg.enabled or self._sock is None:
            return False

        packet = dict(payload)
        packet.setdefault("version", 1)
        packet.setdefault("timestamp_sec", time.time())
        packet.setdefault("seq", self._seq)
        self._seq += 1

        try:
            data = json.dumps(packet, separators=(",", ":")).encode("utf-8")
            if len(data) > self.cfg.max_bytes:
                return False
            self._sock.sendto(data, (self.cfg.host, self.cfg.port))
            return True
        except OSError:
            return False


def tensor_like_to_list(x: Any) -> Any:
    """Convert torch/numpy-like values to python lists without importing torch."""
    if hasattr(x, "detach"):
        x = x.detach()
    if hasattr(x, "cpu"):
        x = x.cpu()
    if hasattr(x, "numpy"):
        x = x.numpy()
    if hasattr(x, "tolist"):
        return x.tolist()
    if isinstance(x, (list, tuple)):
        return [tensor_like_to_list(v) for v in x]
    return x


def build_frame_payload(
    env_id: int,
    dt: float,
    rigid_body_pos: Any,
    rigid_body_rot: Optional[Any] = None,
    predicted_pose: Optional[Any] = None,
    debug_segments: Optional[Iterable[Any]] = None,
    text_prompt: Optional[str] = None,
) -> Dict[str, Any]:
    payload: Dict[str, Any] = {
        "env_id": int(env_id),
        "dt": float(dt),
        "rigid_bodies": {
            "pos": tensor_like_to_list(rigid_body_pos),
        },
    }

    if rigid_body_rot is not None:
        payload["rigid_bodies"]["rot_xyzw"] = tensor_like_to_list(rigid_body_rot)

    if predicted_pose is not None:
        payload["predicted"] = {
            "enabled": True,
            "poses": tensor_like_to_list(predicted_pose),
        }

    if debug_segments is not None:
        payload["debug"] = {
            "segments": tensor_like_to_list(debug_segments),
        }

    if text_prompt is not None:
        payload["text_prompt"] = str(text_prompt)

    return payload
