# CLoSD Hack Log 1 - Prompt XY Continuity (A/B)

Date: 2026-05-17

## Goal

Guarantee that when text prompt changes trigger a new motion horizon, the character starts in the same XY position where it currently is.

## Why It Drifted Before

- New horizon is sampled by MDM and converted to pose space; it is not hard-constrained to live root XY.
- Transition blending smooths motion but does not enforce exact frame-0 root equality.
- Spawn controls are reset-oriented and do not fully control prompt-switch continuity.

## Implemented A/B Toggle

### A mode (baseline)

- Prompt XY lock OFF.
- Behavior remains prior baseline.

### B mode (strict continuity)

- Prompt XY lock ON.
- At prompt switch, compute delta in XY between live root and new horizon frame 0 root.
- Shift full new horizon by this delta in XY (Z unchanged).

## Implemented A/B Toggle 2 (Across Inference Boundaries)

### A mode (baseline)

- Inference continuity lock OFF.
- Each new inferred horizon is consumed with existing behavior.
- Logs: `[CHARACTER POSITION] before ...` and `[CHARACTER POSITION] new ...`

### B mode (hard lock + trajectory bridge)

- Inference continuity lock ON.
- **Hard frame-0 anchor for prompt switches**: When a prompt changes, frame 0 root XY is hard-locked to the live root XY (guarantee exact start position).
- **Velocity bridge for early frames (1..K)**: Over the first N frames, blend generated root trajectory toward extrapolated live-motion path to smooth early transitions.
- **Event-aware**: Separates prompt switches from periodic horizon refreshes for targeted continuity.
- **Diagnostic logging**: Prints `[PROMPT_SWITCH] env=... before ... | new ... | correction_mag=...` showing pre/post correction metrics.

This guarantees frame-0 continuity while smoothing early-horizon path for natural transitions.

## Control Path

1. Viewer button sends UDP command:
	- action: set_prompt_xy_lock
	- enabled: true/false
2. CLoSD command handler updates runtime flag `prompt_xy_lock`.
3. During prompt change in `get_mdm_next_planning_horizon`, if flag ON, apply strict XY alignment.

Additional control path:

1. Viewer button sends UDP command:
	- action: set_inference_continuity_lock
	- enabled: true/false
2. CLoSD updates `inference_continuity_lock`.
3. During every inference boundary, if ON, apply short root XY trajectory bridge.

## Logging

**Mode OFF (baseline):**
- `[CHARACTER POSITION] before x: ..., y: ...`
- `[CHARACTER POSITION] new x: ..., y: ...`

**Mode ON (inference continuity lock):**
- `[PROMPT_SWITCH] env=ID before x: ..., y: ... | new x: ..., y: ... | correction_mag=DELTA`
  - **correction_mag**: Euclidean distance in XY between pre and post-correction new root (should be ~0 after hard lock + bridge).

When lock is ON:
- Frame 0 root XY should match before root exactly.
- Early frames blend toward live motion velocity.
- correction_mag measures the bridge adjustment strength.

## Files Updated

- `pilotlight_integration/closd_overlay/closd/env/tasks/closd.py`
- `pilotlight_integration/pilotlight_app/src/app_bridge.c`

## Safety Notes

- Change is isolated to prompt-switch boundaries.
- Z axis is untouched.
- No changes to reward function or core episode logic.
- A/B switch allows immediate rollback to baseline behavior at runtime.

## Quick Validation Steps

1. Run app and keep Inference Continuity OFF.
2. Trigger prompt changes and observe before/new XY gaps in baseline logging.
3. Toggle Inference Continuity ON in viewer.
4. Trigger prompt changes again.
   - Check: `[PROMPT_SWITCH]` line shows before == new in first frame.
   - Check: `correction_mag` should be small (< 0.1).
5. Watch for smooth root path during early horizon frames (no jitter at transition).
