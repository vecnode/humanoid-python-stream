"""State bridge utilities for external realtime viewers.

This module keeps dependencies minimal and can be imported from task code
without changing planner or policy logic.
"""

from __future__ import annotations

import json
import socket
import time
from pathlib import Path
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


class CharacterJointWrapper:
    """Maps SMPL-24 rigid-body positions onto a custom character rig namespace."""

    def __init__(self, map_path: str = "", enabled: bool = False) -> None:
        self.enabled = bool(enabled)
        self.map_path = str(map_path or "")
        self._smpl24_order = []
        self._mapping = {}
        self._ready = False

        if not self.enabled:
            return

        resolved = self._resolve_map_path(self.map_path)
        if resolved is None:
            print(f"[pilotlight] character wrapper disabled: map not found [{self.map_path}]")
            return

        try:
            payload = json.loads(resolved.read_text(encoding="utf-8"))
            self._smpl24_order = list(payload.get("smpl24_order", []))
            self._mapping = dict(payload.get("mapping", {}))
            self.map_path = str(resolved)
            self._ready = bool(self._smpl24_order and self._mapping)
            if self._ready:
                print(f"[pilotlight] character wrapper enabled with map [{self.map_path}]")
        except (OSError, json.JSONDecodeError, TypeError, ValueError) as exc:
            print(f"[pilotlight] character wrapper disabled: {exc}")

    def _resolve_map_path(self, raw_path: str) -> Optional[Path]:
        if not raw_path:
            return None

        p = Path(raw_path)
        candidates = []
        if p.is_absolute():
            candidates.append(p)
        else:
            candidates.append(Path.cwd() / p)
            candidates.append(Path(__file__).resolve().parents[2] / p)
            candidates.append(Path(__file__).resolve().parents[3] / p)

        for c in candidates:
            if c.exists() and c.is_file():
                return c.resolve()
        return None

    def _midpoint(self, a: Iterable[float], b: Iterable[float]) -> list[float]:
        av = list(a)
        bv = list(b)
        return [0.5 * (av[0] + bv[0]), 0.5 * (av[1] + bv[1]), 0.5 * (av[2] + bv[2])]

    def apply(self, frame_payload: Dict[str, Any]) -> None:
        if not self.enabled or not self._ready:
            return

        rb = frame_payload.get("rigid_bodies", {})
        pos = rb.get("pos", [])
        if not isinstance(pos, list) or len(pos) < len(self._smpl24_order):
            return

        smpl_pos = {name: pos[i] for i, name in enumerate(self._smpl24_order)}
        character_joints: Dict[str, Any] = {}

        for smpl_name in self._smpl24_order:
            spec = self._mapping.get(smpl_name)
            if not isinstance(spec, dict):
                continue

            spec_type = spec.get("type")
            if spec_type == "bone":
                bone_name = spec.get("name")
                if bone_name:
                    character_joints[bone_name] = smpl_pos.get(smpl_name)
            elif spec_type == "midpoint":
                fallback = spec.get("fallback")
                a_name = spec.get("a")
                b_name = spec.get("b")

                left = smpl_name.startswith("L_")
                a_smpl = "L_Elbow" if left else "R_Elbow"
                b_smpl = "L_Hand" if left else "R_Hand"
                if fallback and a_name and b_name and a_smpl in smpl_pos and b_smpl in smpl_pos:
                    character_joints[fallback] = self._midpoint(smpl_pos[a_smpl], smpl_pos[b_smpl])

        if character_joints:
            frame_payload["character_joints"] = character_joints
            frame_payload["character_wrapper"] = {
                "enabled": True,
                "source_map": self.map_path,
                "joint_count": len(character_joints),
            }


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
