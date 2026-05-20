#!/usr/bin/env python3
"""Generate an SMPL-24 wrapper map from FBX string tokens.

This tool does not parse FBX geometry. It scans printable strings inside an
FBX binary and heuristically maps discovered rig names to the SMPL-24 joint
contract used by this repository.
"""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


SMPL24 = [
    "Pelvis",
    "L_Hip",
    "R_Hip",
    "Torso",
    "L_Knee",
    "R_Knee",
    "Spine",
    "L_Ankle",
    "R_Ankle",
    "Chest",
    "L_Toe",
    "R_Toe",
    "Neck",
    "L_Thorax",
    "R_Thorax",
    "Head",
    "L_Shoulder",
    "R_Shoulder",
    "L_Elbow",
    "R_Elbow",
    "L_Wrist",
    "R_Wrist",
    "L_Hand",
    "R_Hand",
]


def _printable_ascii_chunks(data: bytes, min_len: int = 4) -> list[str]:
    chunks: list[str] = []
    current: list[str] = []
    for b in data:
        if 32 <= b <= 126:
            current.append(chr(b))
        else:
            if len(current) >= min_len:
                chunks.append("".join(current))
            current = []
    if len(current) >= min_len:
        chunks.append("".join(current))
    return chunks


def _extract_name_tokens(chunks: list[str]) -> set[str]:
    names: set[str] = set()

    banned_substrings = {
        "fbx",
        "header",
        "version",
        "globalsettings",
        "documents",
        "objectproperties",
        "definitions",
        "relations",
        "connections",
        "takes",
        "rootnodel",
    }

    model_pat = re.compile(r"Model::([^\s\",]+)")
    lr_pat = re.compile(r"\b[a-zA-Z0-9_.]+\.(L|R)\b")
    keyword_pat = re.compile(
        r"\b[a-zA-Z0-9_.]*(spine|pelvis|root|hip|thigh|shin|knee|foot|toe|heel|"
        r"shoulder|arm|forearm|elbow|wrist|hand|neck|head)[a-zA-Z0-9_.]*\b",
        re.IGNORECASE,
    )

    for c in chunks:
        for m in model_pat.findall(c):
            names.add(m)
        for m in lr_pat.findall(c):
            # lr_pat returns only group by default in findall; re-run full match.
            pass
        for m in re.finditer(lr_pat, c):
            names.add(m.group(0))
        for m in re.finditer(keyword_pat, c):
            names.add(m.group(0))

    filtered = set()
    for n in names:
        if len(n) > 64:
            continue
        ln = n.lower()
        if any(b in ln for b in banned_substrings):
            continue
        if ln.endswith("_end"):
            continue
        filtered.add(n)
    return filtered


def _spine_index(name: str) -> int:
    m = re.search(r"spine\.(\d+)", name.lower())
    if m:
        return int(m.group(1))
    if name.lower() == "spine":
        return 0
    return -1


def _pick_first(candidates: set[str], ordered_keywords: list[str]) -> str | None:
    lowered = {c.lower(): c for c in candidates}
    ordered = sorted(lowered.items(), key=lambda it: (len(it[0]), it[0]))
    for kw in ordered_keywords:
        for lc, orig in ordered:
            if lc == kw:
                return orig
        for lc, orig in ordered:
            if kw in lc and ("_" in lc or "." in lc or lc.isalpha()):
                return orig
    return None


def _pick_side(candidates: set[str], side: str, ordered_keywords: list[str]) -> str | None:
    suffix = ".l" if side == "L" else ".r"
    subset = {c for c in candidates if c.lower().endswith(suffix)}
    if not subset:
        # fallback to textual side marker
        marker = "left" if side == "L" else "right"
        subset = {c for c in candidates if marker in c.lower()}
    return _pick_first(subset, ordered_keywords)


def infer_mapping(tokens: set[str]) -> dict[str, dict[str, object]]:
    mapping: dict[str, dict[str, object]] = {}

    spine_names = sorted([t for t in tokens if "spine" in t.lower()], key=_spine_index)

    def spine_at(i: int, fallback: str | None = None) -> str | None:
        if 0 <= i < len(spine_names):
            return spine_names[i]
        return fallback

    pelvis = _pick_first(tokens, ["root", "hips", "pelvis", "spine"])
    torso = spine_at(1, spine_at(0, pelvis))
    spine = spine_at(2, torso)
    chest = spine_at(3, spine)
    neck = _pick_first(tokens, ["neck"]) or spine_at(max(len(spine_names) - 2, 0), chest)
    # Prefer explicit head tokens; fallback to top spine when absent.
    head = _pick_first(tokens, ["head"]) or spine_at(max(len(spine_names) - 1, 0), neck)

    l_hip = _pick_side(tokens, "L", ["thigh", "hip", "pelvis"]) 
    r_hip = _pick_side(tokens, "R", ["thigh", "hip", "pelvis"]) 
    l_knee = _pick_side(tokens, "L", ["shin", "knee", "calf"]) 
    r_knee = _pick_side(tokens, "R", ["shin", "knee", "calf"]) 
    l_ankle = _pick_side(tokens, "L", ["foot", "ankle"]) 
    r_ankle = _pick_side(tokens, "R", ["foot", "ankle"]) 
    l_toe = _pick_side(tokens, "L", ["toe"]) 
    r_toe = _pick_side(tokens, "R", ["toe"]) 

    l_thorax = _pick_side(tokens, "L", ["shoulder", "clav", "collar", "breast"])
    r_thorax = _pick_side(tokens, "R", ["shoulder", "clav", "collar", "breast"])
    l_shoulder = _pick_side(tokens, "L", ["upper_arm", "arm"])
    r_shoulder = _pick_side(tokens, "R", ["upper_arm", "arm"])
    l_elbow = _pick_side(tokens, "L", ["forearm", "elbow"])
    r_elbow = _pick_side(tokens, "R", ["forearm", "elbow"])
    l_hand = _pick_side(tokens, "L", ["hand", "wrist"])
    r_hand = _pick_side(tokens, "R", ["hand", "wrist"])

    mapping["Pelvis"] = {"type": "bone", "name": pelvis}
    mapping["L_Hip"] = {"type": "bone", "name": l_hip}
    mapping["R_Hip"] = {"type": "bone", "name": r_hip}
    mapping["Torso"] = {"type": "bone", "name": torso}
    mapping["L_Knee"] = {"type": "bone", "name": l_knee}
    mapping["R_Knee"] = {"type": "bone", "name": r_knee}
    mapping["Spine"] = {"type": "bone", "name": spine}
    mapping["L_Ankle"] = {"type": "bone", "name": l_ankle}
    mapping["R_Ankle"] = {"type": "bone", "name": r_ankle}
    mapping["Chest"] = {"type": "bone", "name": chest}
    mapping["L_Toe"] = {"type": "bone", "name": l_toe}
    mapping["R_Toe"] = {"type": "bone", "name": r_toe}
    mapping["Neck"] = {"type": "bone", "name": neck}
    mapping["L_Thorax"] = {"type": "bone", "name": l_thorax}
    mapping["R_Thorax"] = {"type": "bone", "name": r_thorax}
    mapping["Head"] = {"type": "bone", "name": head}
    mapping["L_Shoulder"] = {"type": "bone", "name": l_shoulder}
    mapping["R_Shoulder"] = {"type": "bone", "name": r_shoulder}
    mapping["L_Elbow"] = {"type": "bone", "name": l_elbow}
    mapping["R_Elbow"] = {"type": "bone", "name": r_elbow}

    # Wrist can be synthesized as midpoint when the rig has no explicit wrist bone.
    mapping["L_Wrist"] = {
        "type": "midpoint",
        "a": l_elbow,
        "b": l_hand,
        "fallback": l_hand,
    }
    mapping["R_Wrist"] = {
        "type": "midpoint",
        "a": r_elbow,
        "b": r_hand,
        "fallback": r_hand,
    }
    mapping["L_Hand"] = {"type": "bone", "name": l_hand}
    mapping["R_Hand"] = {"type": "bone", "name": r_hand}

    return mapping


def main() -> int:
    parser = argparse.ArgumentParser(description="Build SMPL-24 wrapper mapping from FBX strings")
    parser.add_argument("--fbx", required=True, help="Path to input FBX")
    parser.add_argument("--out", required=True, help="Path to output JSON map")
    args = parser.parse_args()

    fbx_path = Path(args.fbx).resolve()
    out_path = Path(args.out).resolve()

    if not fbx_path.exists():
        raise FileNotFoundError(f"FBX not found: {fbx_path}")

    data = fbx_path.read_bytes()
    chunks = _printable_ascii_chunks(data)
    tokens = _extract_name_tokens(chunks)
    mapping = infer_mapping(tokens)

    unresolved = []
    for j in SMPL24:
        src = mapping[j]
        if src["type"] == "bone" and not src.get("name"):
            unresolved.append(j)
        if src["type"] == "midpoint" and not src.get("a") and not src.get("b") and not src.get("fallback"):
            unresolved.append(j)

    payload = {
        "source_fbx": str(fbx_path),
        "smpl24_order": SMPL24,
        "mapping": mapping,
        "unresolved_joints": unresolved,
        "candidate_tokens_count": len(tokens),
        "candidate_tokens": sorted(tokens),
        "notes": [
            "Auto-generated using FBX string heuristics.",
            "Validate in Blender before production export.",
        ],
    }

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(f"Wrote mapping: {out_path}")
    if unresolved:
        print("Unresolved joints:", ", ".join(unresolved))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
