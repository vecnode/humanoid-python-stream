# humanoid-python-stream

Text-to-motion simulation. Combines [CLoSD](https://github.com/GuyTevet/CLoSD), [Isaac Gym](https://developer.nvidia.com/isaac-gym), and [PilotLight](https://github.com/themaxmarchuk/pilotlight) through a bridge so prompts can drive a humanoid and stream the state to a real-time viewer.

- Runs CLoSD motion generation with Isaac Gym physics.
- Integration layer between simulation and the PilotLight app.


## Quick launch

```bash
# Build viewer (once)
cd pilotlight_integration/pilotlight_app
./build_bridge_viewer.sh
```

```bash
conda activate closd
cd pilotlight_integration
./launch_closd_pilotlight.sh
# optional: --build-viewer --debug-hml --keep-viewer
```

## Setup

### 1. Prerequisites (Ubuntu/Linux)

Install system packages used by Isaac Gym and PilotLight build:

```bash
sudo apt update
sudo apt install -y \
  build-essential clang cmake ninja-build pkg-config \
  libx11-dev libxrandr-dev libxi-dev libxinerama-dev libxcursor-dev \
  libgl1-mesa-dev libvulkan-dev glslang-tools
```

Install Miniconda or Conda if not already installed.

### 2. Repository layout

Expected root folders:

- CLoSD
- isaacgym
- pilotlight_integration

### 3. Create environment

Isaac Gym prebuilt binaries support Python 3.6-3.8, so this repo uses Python 3.8.

```bash
conda env create -f environment.yml
conda activate closd
```

### 4. Install required local-source packages

```bash
# IsaacGym (from NVIDIA package)
pip install isaacgym/python/

# Optional: fix missing gymtorch.cpp path if needed
ln -sfn "$PWD/isaacgym/python/isaacgym/_bindings/src" \
  "$CONDA_PREFIX/lib/python3.8/site-packages/isaacgym/_bindings/src"

# pytorch3d
export CUDA_HOME=/usr/local/cuda-12
export PATH="$CUDA_HOME/bin:$PATH"
export LD_LIBRARY_PATH="$CUDA_HOME/lib64:${LD_LIBRARY_PATH:-}"
pip install "git+https://github.com/facebookresearch/pytorch3d.git@v0.7.9"

# CLIP
pip install git+https://github.com/openai/CLIP.git
```

## Key models

| Model | Path | Role |
|---|---|---|
| DiP diffusion planner | [CLoSD/closd/diffusion_planner/save/DiP_no-target_10steps_context20_predict40/model000200000.pt](CLoSD/closd/diffusion_planner/save/DiP_no-target_10steps_context20_predict40/model000200000.pt) | Creates future motion from text |
| CLoSD RL policy | [CLoSD/output/CLoSD/CLoSD_t2m_finetune/Humanoid.pth](CLoSD/output/CLoSD/CLoSD_t2m_finetune/Humanoid.pth) | Tracks planned motion with physics control |
| SMPL body model | [CLoSD/closd/diffusion_planner/body_models/smpl/SMPL_NEUTRAL.pkl](CLoSD/closd/diffusion_planner/body_models/smpl/SMPL_NEUTRAL.pkl) | Body representation used in planning and control |

