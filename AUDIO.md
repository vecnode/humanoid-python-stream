# Audio Model Integration Plan (VibeVoice Realtime)

## Goal

Instantiate `microsoft/VibeVoice-Realtime-0.5B` during application startup so the model is loaded and warm, but do not change any current CLoSD + PilotLight behavior yet.

Scope for this phase:

- add a separate Python audio stack (outside CLoSD code)
- load model at startup
- expose a ready interface for future pose/phase inputs
- no audio synthesis output wired yet

## Current Startup Path (where to hook)

Today startup is:

1. `pilotlight_integration/launch_closd_pilotlight.sh`
2. starts PilotLight viewer
3. runs `python3 closd/run.py ...`
4. CLoSD `main()` initializes dependencies, env, runner, then executes `runner.run(cfg)`

Best hook point to keep everything stable:

- initialize audio runtime in `CLoSD/closd/run.py` inside `main()` after config is resolved and before `runner.run(cfg)`.

This guarantees model load happens once per run and is ready before simulation loop begins.

## Proposed New Structure (separate from CLoSD internals)

Create a new top-level folder:

- `audio_runtime/`

Planned files:

- `audio_runtime/__init__.py`
- `audio_runtime/config.py`
- `audio_runtime/runtime.py`
- `audio_runtime/model_loader.py`
- `audio_runtime/queue.py` (future input buffering)
- `audio_runtime/README.md` (optional internal notes later)

Rationale:

- keeps audio code isolated from motion/control stack
- enables independent package/dependency management
- avoids mixing with CLoSD training/runtime internals

## Startup Integration Plan

### Step 1: Add an audio bootstrap in CLoSD entrypoint

In `CLoSD/closd/run.py`:

- import a lightweight bootstrap function, for example `from audio_runtime.runtime import initialize_audio_runtime`
- call it once in `main()` before `runner.run(cfg)`
- store returned handle in a local variable (or module global) to keep runtime alive

Behavior for this phase:

- if audio init fails, log warning and continue running normally (non-fatal)
- no changes to env stepping, rewards, control, or viewer bridge

### Step 2: Add launcher toggles (optional but recommended)

In `pilotlight_integration/launch_closd_pilotlight.sh` pass runtime flags through Hydra overrides, e.g.:

- `audio.enabled=True`
- `audio.device=cuda:0`
- `audio.model_id=microsoft/VibeVoice-Realtime-0.5B`
- `audio.preload_only=True`

Default values should preserve current behavior if flags are absent.

### Step 3: Add config block

In CLoSD config tree (`CLoSD/closd/data/cfg/...`) add an `audio` section:

- `enabled: true`
- `model_id: microsoft/VibeVoice-Realtime-0.5B`
- `device: cuda:0`
- `dtype: float16`
- `cache_dir: ~/.cache/huggingface`
- `preload_only: true`
- `warmup_seconds: 0.25`

This keeps model selection and runtime mode fully configurable.

## Model Loading Plan

`audio_runtime/model_loader.py` responsibilities:

1. resolve model source (`microsoft/VibeVoice-Realtime-0.5B`)
2. download/cache with Hugging Face cache (reuse existing environment behavior)
3. load tokenizer/processor/model objects required by VibeVoice
4. move model to configured device
5. run a minimal warmup pass so first real request has low latency

Notes:

- use `torch.inference_mode()`
- do not emit audio to disk/device in this phase
- only validate the forward path with a tiny dummy input

## Runtime Interface (ready for inputs, no synthesis wiring yet)

`audio_runtime/runtime.py` should expose something like:

- `initialize_audio_runtime(cfg) -> AudioRuntime | None`
- `AudioRuntime.is_ready() -> bool`
- `AudioRuntime.submit_conditioning(pose_tensor, phase_tensor, meta)` (store/validate only for now)
- `AudioRuntime.shutdown()`

For this phase, `submit_conditioning(...)` can be a no-op buffer path to prove interface readiness without affecting current loop.

## Dependency Plan

Because this is a separate stack, keep requirements isolated.

Add one of:

- `audio_runtime/requirements.txt`
- or `audio_runtime/pyproject.toml`

Expected packages (exact versions to be pinned during implementation):

- `torch` (already present in closd env)
- `transformers`
- `huggingface_hub`
- `safetensors`
- any VibeVoice-specific package if required by upstream model docs

Install strategy:

- install into existing `closd` conda env for now
- keep package list isolated so audio deps are auditable

## Validation Checklist (no behavior changes)

After implementation, confirm:

1. existing startup still works (`launch_closd_pilotlight.sh`)
2. logs show audio model preload success and device placement
3. simulation/viewer behavior remains unchanged
4. on preload failure, app continues with warning only
5. no extra network downloads after first successful cache fill

## Suggested Logging

At startup:

- `[audio] enabled=True model=microsoft/VibeVoice-Realtime-0.5B device=cuda:0`
- `[audio] loading model...`
- `[audio] warmup complete; ready=True`

On failure:

- `[audio] init failed; continuing without audio: <error>`

## Phase Boundary

This plan intentionally stops at "model loaded and ready".

Not included yet:

- mapping live pose/phase features to model conditioning
- streaming PCM/audio output
- playback in viewer or separate audio sink
- synchronization policy between physics tick and audio frame cadence
