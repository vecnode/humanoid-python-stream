# mixed-motion

Under heavy development


## Launch

```bash
# Build viewer (once)
cd pilotlight_integration/pilotlight_app
./build_bridge_viewer.sh
```

```bash
conda activate closd
cd pilotlight_integration
./launch_closd_pilotlight.sh
# optional: --build-viewer  --debug-hml  --keep-viewer
```

## From scratch

### 1. Prerequisites (Ubuntu/Linux)

Install system packages used by Isaac Gym + PilotLight build:

```bash
sudo apt update
sudo apt install -y \
        build-essential clang cmake ninja-build pkg-config \
        libx11-dev libxrandr-dev libxi-dev libxinerama-dev libxcursor-dev \
        libgl1-mesa-dev libvulkan-dev glslang-tools
```

Install Miniconda/Conda if not already installed.

### 2. Repository layout

This project expects these folders at the root level:

- CLoSD
- isaacgym
- pilotlight_integration




## First Install

IsaacGym ships pre-compiled binaries **only for Python 3.6–3.8**, so this project is configured around Python 3.8.

| Environment | Python | Purpose |
|---|---|---|
| `conda closd` | **3.8** | IsaacGym · CLoSD · DiP · RL policy · SMPL · BERT |

### 1. Create the conda environment (Python 3.8 — sim + all models)

```bash
conda env create -f environment.yml   # creates env named "closd"
conda activate closd
```

Then install the three packages that require local source or CUDA headers:

```bash
# IsaacGym — download the .tar.gz from https://developer.nvidia.com/isaac-gym,
# extract it, then install the bundled wheel:
pip install isaacgym/python/

# If launch later fails with missing gymtorch.cpp, link IsaacGym sources:
ln -sfn "$PWD/isaacgym/python/isaacgym/_bindings/src" \
        "$CONDA_PREFIX/lib/python3.8/site-packages/isaacgym/_bindings/src"

# pytorch3d — build from source. Ensure nvcc matches torch CUDA (torch=cu121 here):
export CUDA_HOME=/usr/local/cuda-12
export PATH="$CUDA_HOME/bin:$PATH"
export LD_LIBRARY_PATH="$CUDA_HOME/lib64:${LD_LIBRARY_PATH:-}"
nvcc --version  # should report CUDA 12.x (not 11.x)
pip install "git+https://github.com/facebookresearch/pytorch3d.git@v0.7.9"

# CLIP:
pip install git+https://github.com/openai/CLIP.git
```






### 5. Build the PilotLight bridge viewer

```bash
cd pilotlight_integration/pilotlight_app
./build_bridge_viewer.sh
```

### 6. Run end-to-end

```bash
conda activate closd
cd pilotlight_integration
./launch_closd_pilotlight.sh
# optional: --build-viewer --debug-hml --keep-viewer
```

### 7. Where to place custom edits

External repos are treated as dependencies. Keep your editable sources here:

- PilotLight app bridge source: pilotlight_integration/pilotlight_app/src/app_bridge.c
- CLoSD overlay files: pilotlight_integration/closd_overlay/...

The launcher runs pilotlight_integration/sync_closd_overlay.sh before starting CLoSD, so overlay files are copied into CLoSD automatically.



## Models


| Model | Path | Role |
|---|---|---|
| DiP diffusion planner (text-to-motion, no target) | [model000200000.pt](CLoSD/closd/diffusion_planner/save/DiP_no-target_10steps_context20_predict40/model000200000.pt) | Predicts future motion from text prompt, 10 DDIM steps, 20 context / 40 predicted frames |
| CLoSD RL policy | [Humanoid.pth](CLoSD/output/CLoSD/CLoSD_t2m_finetune/Humanoid.pth) | AMP-based physics RL controller that drives the humanoid to track DiP predictions |
| SMPL body model | [SMPL_NEUTRAL.pkl](CLoSD/closd/diffusion_planner/body_models/smpl/SMPL_NEUTRAL.pkl) | Body shape / joint regression used by both systems |
| BERT text encoder | `distilbert-base-uncased` via HuggingFace cache (`~/.cache/huggingface/`) | Encodes the text prompt into an embedding that conditions DiP |


## AI DSP

### Global Architecture

The runtime behaves like a continuous AI DSP loop: prompts are streamed in, transformed into motion plans, stabilized by physics control, and rendered by the PilotLight viewer while new prompts keep updating intent.

```mermaid
flowchart LR
        P([Prompt Stream<br/>text intent · continuous])

        subgraph C[CLoSD AI Core]
                direction TB

                E[Text Encoder]
                Em["distilbert-base-uncased"]:::model

                D[DiP Diffusion Planner<br/>10 DDIM steps · 20 ctx → 40 pred frames]
                Dm["model000200000.pt"]:::model

                R[CLoSD RL Policy<br/>AMP-based tracking controller]
                Rm["Humanoid.pth"]:::model

                X[Physics Simulation<br/>Isaac Gym · joint torques · collisions]
                Xm["SMPL_NEUTRAL.pkl"]:::model

                E -.->|weights| Em
                D -.->|weights| Dm
                R -.->|weights| Rm
                X -.->|body model| Xm
        end

        subgraph A[PilotLight App]
                direction TB
                B[IPC Bridge<br/>shared-memory · debug channels]
                G[Graphics Renderer<br/>skeleton mesh · scene overlays]
        end

        P --> E --> D --> R --> X
        X --> B --> G
        X -. state feedback .-> D
        G -- user observes · next prompt --> P

        classDef model fill:#f4f4f4,stroke:#bbb,color:#555,font-size:11px
        classDef default rx:6
```

### DiP and RL Closed Loop

This diagram shows how text conditioned planning and physics control operate as one loop during runtime. The next two sections then zoom into each side separately.

```mermaid
flowchart LR
        I0[Pipeline input<br/>user prompt stream] --> T
        T[Prompt text input] --> E[Text embedding]
        E --> P[DiP planner]
        P --> H[Predicted motion horizon]

        H --> R[RL tracking policy]
        R --> S[Isaac Gym simulation]
        S --> X[Live humanoid state]

        X --> O[Imitation observations]
        H --> O
        O --> R

        X --> W[Imitation reward]
        H --> W
        W --> R

        S --> C[Pose context buffer]
        C --> P

        X --> O0[Pipeline outputs<br/>viewer state stream and control state]
        H --> O1[Planner output contract<br/>next target pose and horizon]
```

### DiP Diffusion Planner

The DiP planner runs as a prefix-completion diffusion pipeline inside the CLoSD task. At each planning boundary, it builds context from recent simulated pose, conditions on the current text prompt, samples a horizon in HumanML space, converts back to SMPL XYZ, and returns the next planning window for tracking.

```mermaid
flowchart LR
        I1[Inputs from closed loop<br/>prompt text and pose context buffer] --> A
        A[Sim pose buffer<br/>pose_buffer @ 30 FPS] --> B[build_completion_input<br/>pose_to_hml + prefix/mask]
        P[Prompt list<br/>hml_prompts] --> C[get_text_prompts]
        C --> D[text cache check]
        D -->|changed| E[mdm.encode_text]
        D -->|unchanged| F[reuse cached text_embed]
        E --> G[model_kwargs.y]
        F --> G
        B --> G

        G --> H[diffusion.p_sample_loop<br/>sample_fn]
        H --> I[cur_mdm_pred<br/>HumanML sample]
        I --> J[rep.hml_to_pose<br/>20 FPS -> 30 FPS]
        J --> K[extract planning_horizon]
        K --> L[transition blending<br/>prompt-switch smoothing]
        L --> M[inference continuity lock<br/>root XY bridge]
        M --> N[get_pred_pose returns<br/>j3d + optional j3d_prev]

        N --> O2[Output to RL graph<br/>reference pose packet for tracking]
        K --> O3[Output to bridge graph<br/>predicted horizon for visualization]
```

Implementation anchors: `CLoSD.get_mdm_next_planning_horizon`, `CLoSD.build_completion_input`, `RepresentationHandler.hml_to_pose`, and `CLoSD.get_pred_pose`.

### RL Humanoid

The RL humanoid loop uses AMP-style policy control in Isaac Gym. The policy acts each step, simulation advances physics, then imitation observations/rewards are computed against DiP-generated reference pose and velocity.

```mermaid
flowchart LR
        I2[Inputs from DiP graph<br/>reference pose packet and horizon] --> G
        A[Policy step<br/>AMPAgent play_steps_rnn] --> B[Environment step<br/>env_step with actions]
        B --> C[Pre physics step<br/>HumanoidAMP pre_physics_step]
        C --> D[Isaac Gym simulation]
        D --> E[Post physics step<br/>CLoSD post_physics_step]
        E --> F[Pose buffer update and frame index advance]

        G[Planner query<br/>get_pred_pose] --> H[Generated reference state<br/>get_state_from_gen_cache]
        H --> I[Reference tensors<br/>ref rb pos and ref body vel]
        I --> J[Task observation build<br/>compute_task_obs_demo]
        D --> J
        J --> K[Imitation observation kernel<br/>compute_imitation_observations_v7]
        K --> L[Task observation output]

        D --> M[Imitation reward kernel<br/>compute_imitation_reward_wo_rot]
        I --> M
        M --> N[Reward output<br/>reward and reward_raw]

        L --> O[Next observation package]
        N --> O
        O --> A

        D --> O4[Outputs to global runtime<br/>rigid body state and simulation dt]
```

This closes the tracking loop: planner predictions become reference motion, and the RL controller is rewarded for matching body position and velocity while maintaining stable simulation.

