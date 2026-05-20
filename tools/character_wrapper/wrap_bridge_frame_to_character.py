#!/usr/bin/env python3
"""Map BridgeFrame SMPL-24 positions to custom character rig joint names.

Input JSON is expected to follow the existing payload contract used by
closd.utils.viewer_bridge.build_frame_payload with rigid_bodies.pos.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def _vec3_add(a: list[float], b: list[float]) -> list[float]:
    return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]


def _vec3_mul(a: list[float], s: float) -> list[float]:
    return [a[0] * s, a[1] * s, a[2] * s]


def _midpoint(a: list[float], b: list[float]) -> list[float]:
    return _vec3_mul(_vec3_add(a, b), 0.5)


def main() -> int:
    parser = argparse.ArgumentParser(description="Wrap SMPL24 bridge frame to character joints")
    parser.add_argument("--map", required=True, help="Path to character*_smpl24_wrapper.json")
    parser.add_argument("--frame", required=True, help="Path to bridge frame JSON input")
    parser.add_argument("--out", required=True, help="Path to output wrapped frame JSON")
    args = parser.parse_args()

    map_data = json.loads(Path(args.map).read_text(encoding="utf-8"))
    frame = json.loads(Path(args.frame).read_text(encoding="utf-8"))

    smpl_order = map_data["smpl24_order"]
    mapping = map_data["mapping"]

    rb = frame.get("rigid_bodies", {})
    pos = rb.get("pos", [])

    if len(pos) < len(smpl_order):
        raise ValueError(
            f"Expected at least {len(smpl_order)} joints in rigid_bodies.pos, got {len(pos)}"
        )

    smpl_pos = {name: pos[i] for i, name in enumerate(smpl_order)}

    character_joints: dict[str, list[float]] = {}
    for smpl_name in smpl_order:
        spec = mapping[smpl_name]
        if spec["type"] == "bone":
            bone = spec.get("name")
            if bone:
                character_joints[bone] = smpl_pos[smpl_name]
        elif spec["type"] == "midpoint":
            a = spec.get("a")
            b = spec.get("b")
            fallback = spec.get("fallback")
            if a and b:
                va = smpl_pos.get("L_Elbow") if a.endswith(".L") else smpl_pos.get("R_Elbow")
                vb = smpl_pos.get("L_Hand") if b.endswith(".L") else smpl_pos.get("R_Hand")
                if va is not None and vb is not None and fallback:
                    character_joints[fallback] = _midpoint(va, vb)

    out_payload = dict(frame)
    out_payload["character_joints"] = character_joints
    out_payload["character_wrapper"] = {
        "source_map": str(Path(args.map).resolve()),
        "joint_count": len(character_joints),
    }

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(out_payload, indent=2), encoding="utf-8")
    print(f"Wrote wrapped frame: {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
