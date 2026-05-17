# Character Loading & Visualization Plan

## Overview

This document outlines the plan for loading and displaying a humanoid character in the PilotLight visualizer instead of just spheres representing joint positions.

## Current State

### Visualization Architecture
- **Bridge Communication**: CLoSD sends motion data to PilotLight via network socket
  - Protocol: `BridgeFrame` structure (defined in `app_bridge.c`) containing:
    - `uBodyCount`: Number of joint positions
    - `atBodyPos[MAX_BODIES]`: 3D positions of all skeleton joints
    - `uPredCount`: Predicted joint positions
    - `atPredPos[MAX_PRED_POINTS]`: Predicted positions for look-ahead
  - Network: localhost:45678 (motion data), localhost:45679 (control)

- **Current Rendering**: 
  - Yellow spheres (radius 0.04m) for body joints when `bShowYellow` flag enabled
  - Blue skeletal lines connecting joints (hardcoded SMPL 24-joint connectivity)
  - Green spheres for predicted positions when `bShowGreen` flag enabled
  - Located in: `pilotlight_integration/pilotlight_app/src/app_bridge.c` (~line 883)

### Skeleton Structure
- **Format**: SMPL (Skinned Multi-Person Linear) humanoid model
- **Joint Count**: 24 joints total
- **Joint Names**: 
  ```
  Pelvis, L_Hip, L_Knee, L_Ankle, L_Toe, R_Hip, R_Knee, R_Ankle, R_Toe,
  Torso, Spine, Chest, Neck, Head,
  L_Thorax, L_Shoulder, L_Elbow, L_Wrist, L_Hand,
  R_Thorax, R_Shoulder, R_Elbow, R_Wrist, R_Hand
  ```
- **Assets Location**: `CLoSD/closd/data/assets/`
  - MJCF files: `mjcf/smpl_humanoid.xml`, `mesh_humanoid.xml`
  - Mesh directory: `mesh/smpl/` (cached at runtime with UUIDs)
  - FBX files: `fbx/` (environment objects only)

### Available Libraries & Tools
- **SMPL Robot Classes**: `CLoSD/closd/utils/smpllib/smpllib/smpl_local_robot.py`
  - `SMPL_Robot` class handles SMPL model loading, mesh generation, physics
  - Can generate geometry (STL meshes) for each joint from SMPL vertices
  - Supports export visualization strings with mesh references

- **Skeleton Utilities**: `CLoSD/closd/utils/smpllib/khrylib/mocap/skeleton_mesh_local.py`
  - `Skeleton` / `SkeletonMesh` classes for skeleton tree manipulation
  - Can construct and write XML-based MJCF descriptions
  - Supports mesh, capsule, and box geometry per bone

- **Blender Integration**: `CLoSD/closd/blender/blender_utils.py`
  - `xml2mesh()`: Converts MJCF descriptions to OBJ/mesh files
  - Automated mesh generation from SMPL parameters
  - Can generate collision hulls and convex geometries

## Character Data Input Requirements

### Option A: SMPL-Based Characters (Recommended - Automatic)
If you have or want to use **SMPL model parameters**, the system automatically generates all geometry:

**Input**:
- SMPL body shape parameters (betas: 10 values)
- SMPL pose parameters (optional, for reference pose)
- Gender (male/female/neutral)

**System Generates**:
- 24 joint meshes from SMPL vertices
- Automatic mesh generation via `smpl_local_robot.py`
- Cached in: `CLoSD/closd/data/assets/mesh/smpl/{uuid}/`

**Advantage**: One mesh fits all SMPL variations (body shapes, poses)

---

### Option B: Custom Full-Body Character Mesh (Manual - Requires Processing)

If you have a **pre-made humanoid character mesh**:

**Short answer for your current assets (FBX + GLTF + textures)**:
- **Yes, you should rig in Blender (or another DCC) if the model is not already rigged to a humanoid skeleton.**
- **No, dropping in a GLTF file alone will not make this current CLoSD -> bridge pipeline animate correctly.**
- In the current design, motion comes in as **24 joint positions**; without a matching rig/skeleton mapping, the character cannot follow those joints.

**What GLTF support means here**:
- PilotLight may be able to **load and render** a `.gltf/.glb` mesh, materials, and textures.
- That is **not the same** as being fully driven by CLoSD motion data.
- For the character to move with your stream, you still need either:
  - a per-joint mesh workflow (current plan: one mesh per SMPL joint), or
  - a future skinning workflow (single skinned mesh + bone transforms), which is not the implemented path in this document.

**Input Format Options**:
1. **FBX Rigged Character** (best source format for editing)
    - File: `character.fbx`
    - Must include skeleton (armature) and skin weights
    - Preferred for retargeting and fixing bones in Blender

2. **GLTF/GLB Rigged Character** (render-friendly delivery format)
    - File: `character.gltf` or `character.glb`
    - Include texture files (or embed textures in `.glb`)
    - Can be used for display assets, but still requires correct rig mapping for motion-driven use

3. **OBJ + Armature in Blender**
    - File: `character.obj` (mesh)
    - File: `character.blend` (rigging)
    - Valid if you complete rigging/weighting in Blender

4. **COLLADA DAE**
    - File: `character.dae`
    - Includes mesh + skeleton if exported that way

**Mandatory rig requirements (for motion-driven character)**:
- Exactly 24 SMPL-compatible bones in the expected hierarchy
- Exact bone names (case-sensitive) matching the SMPL list used in this repo
- Skin weights painted so each body region is influenced by the correct bones
- Rest pose in T-pose or A-pose

**Size & Scale Requirements**:
- **Unit**: Meters
- **Height**: ~1.7m (human-like proportions)
- **Standing Pose**: T-pose or A-pose with arms extended
- **Coordinate System**: Z-up (matches CLoSD/Isaac)
- **Scale Verification**: Shoulder width ~0.5m, foot length ~0.25m

**Explicit workflow for your case (FBX/GLTF + textures)**:

1. **Open FBX in Blender and verify rig quality**:
    - If already rigged, validate bone names + hierarchy + weights
    - If not rigged, add/retarget to the 24-bone SMPL-compatible skeleton
    - Fix orientation and scale before export

2. **Keep textures organized**:
    - If using `.gltf`, keep referenced texture paths valid relative to the `.gltf` file
    - If using `.glb`, confirm textures are embedded and load correctly

3. **Decide the runtime path**:
    - **Current implementation path (recommended now)**: convert to per-joint meshes (`{bone_name}.stl`) for each of the 24 joints
    - **Future path (not covered by current rendering code)**: single skinned GLTF mesh with shader skinning driven by bone transforms

4. **For current path, export per-joint geometry**:
    - Split mesh by bone influence in Blender
    - Export one file per joint: `Pelvis.stl`, `L_Hip.stl`, ..., `R_Hand.stl`
    - Ensure all 24 files exist and match exact joint names

5. **Generate/validate character config**:
    - Create `config.json` with joint order, colors, scale, and mesh file paths
    - Ensure index order matches incoming bridge joint order exactly

6. **Test in PilotLight**:
    - First test static loading (mesh appears with correct textures/material intent)
    - Then test streamed motion (joints update and mesh pieces follow correctly)
    - Keep sphere fallback enabled until motion-driven mesh rendering is validated

**How to answer "is this enough to update GLTF and it moves?"**:
- **If you only replace a static GLTF asset**: it will likely render, but not move correctly with CLoSD joint stream.
- **If you also provide proper rig mapping and the rendering path that consumes joint/bone transforms**: yes, it can move.
- In this document's current pipeline, that means preparing the 24-joint compatible assets and mapping them explicitly.

**Processing Steps** (to convert to per-joint STL files):

1. **Export from Blender/Unreal/Maya**:
    - Export full body mesh + skeleton
    - Verify bones match SMPL 24-joint structure:
      ```
      Pelvis (root)
      ├── L_Hip → L_Knee → L_Ankle → L_Toe
      ├── R_Hip → R_Knee → R_Ankle → R_Toe
      ├── Torso → Spine → Chest → Neck → Head
      └── Shoulders → Elbows → Wrists → Hands (left/right)
      ```

2. **Split Mesh by Bones** (using provided tools):
    - Use `CLoSD/closd/blender/obj_utils.py::xml2mesh()` to convert MJCF -> mesh
    - Or write custom script to extract mesh portions for each bone
    - Script should:
      - Identify which vertices belong to each bone (via skin weights)
      - Extract sub-mesh for each bone
      - Export as `{bone_name}.stl`

3. **Generate Convex Hulls** (simplification):
    - Option: Create convex hull per bone for better performance
    - Use `scipy.spatial.ConvexHull` or Blender's built-in convex hull
    - Reduces vertices while maintaining collision shape

**Example Blender Script** (to split mesh):
```python
import bpy
import numpy as np
from stl import mesh as stl_mesh

# Assume: Active object has mesh + skeleton
obj = bpy.context.active_object
armature = None  # Find parent armature

for bone in armature.data.bones:
    # Select vertices weighted to this bone
    verts_for_bone = []
    for vert in obj.data.vertices:
        for group in vert.groups:
            if armature.vertex_groups[group.group].name == bone.name:
                verts_for_bone.append(vert)
    
    # Export submesh as STL
    submesh = extract_mesh(obj, verts_for_bone)
    submesh.save(f"meshes/{bone.name}.stl")
```

---

### Option C: Simple Capsule/Box Skeleton (Minimal - Fast Setup)

If you just want a **skeletal representation** without detailed mesh:

**Input**:
- 24 joint positions
- Per-joint size (radius or box dimensions)

**System Generates**:
- Capsules or boxes per joint
- No complex mesh processing needed

**Files to Create**:
```json
{
  "joints": [
    {"name": "Pelvis", "radius": 0.08},
    {"name": "L_Hip", "radius": 0.06},
    {"name": "L_Knee", "radius": 0.05},
    // ... 21 more joints
  ]
}
```

---

## Implementation Plan

### Phase 1: Character Data Preparation
**Goal**: Generate mesh geometry for the humanoid skeleton

**Steps**:
1. **Generate SMPL Mesh Geometry**
   - Location: `CLoSD/closd/data/assets/mesh/smpl/humanoid/`
   - Use `smpl_local_robot.py::SMPL_Robot.load_from_skeleton()` to:
     - Load default SMPL model (zero pose)
     - Generate convex hull geometry for each joint (24 STL files, one per joint)
     - Export as: `{joint_name}.stl` (e.g., `Pelvis.stl`, `L_Hand.stl`)
   - Output STL files per joint with proper vertex weighting

2. **Create Character Configuration File**
   - Location: `CLoSD/closd/data/assets/characters/default_humanoid/`
   - File structure:
     ```
     default_humanoid/
     ├── config.json          # metadata, scale, colors, collision groups
     ├── skeleton.xml         # MJCF structure (joints, hierarchy)
     └── meshes/
         ├── Pelvis.stl
         ├── L_Hand.stl
         ├── R_Hand.stl
         ├── Head.stl
         └── ... (22 more joints)
     ```
   - Config should include:
     - Joint names and order
     - Bone colors (RGBA per joint)
     - Scale factors (if needed)
     - Collision groups/filtering
     - LOD settings (simple/detailed geometry options)

### Phase 2: Extend Bridge Protocol
**Goal**: Send character/mesh metadata alongside joint positions

**Modifications to `BridgeFrame` struct** (in `app_bridge.c`):
```c
typedef struct _BridgeCharacterMeta {
    char acCharacterName[128];      // e.g., "default_humanoid"
    uint32_t uJointCount;
    char acJointNames[256][32];     // joint names in order
    float afJointRadii[256];        // per-joint collision radius
    // Optionally: texture path, color palette, LOD level
} BridgeCharacterMeta;

// Add to BridgeFrame:
bool bHasCharacterMeta;
BridgeCharacterMeta tCharacterMeta;
```

**CLoSD Sender Update** (Python side):
- When starting inference, send character metadata once
- Currently sends in: `closd/run.py` or bridge interface module
- Add call to send character info packet before motion frames

### Phase 3: Rendering Backend - Mesh Loading
**Goal**: Load and cache character mesh data in the visualizer

**Implementation in `app_bridge.c`**:

1. **Add Mesh Cache Structure**:
   ```c
   typedef struct _CharacterModel {
       char acName[128];
       uint32_t uJointCount;
       char acJointNames[256][32];
       plMesh* atMeshes[256];        // One mesh per joint
       plVec4 atJointColors[256];    // RGBA per joint
   } CharacterModel;
   ```

2. **Add Mesh Loading Function**:
   - Function: `pl__load_character_model(const char* acCharacterPath)`
   - Load from filesystem:
     - Read config JSON for metadata
     - Load STL files for each joint mesh
     - Cache meshes in GPU memory
   - Use existing PilotLight mesh loading: `plAssetLoaderI::load_mesh()`

3. **Integrate with App Data**:
   ```c
   typedef struct _plAppData {
       // ... existing fields ...
       CharacterModel tCharacter;
       bool bCharacterLoaded;
   } plAppData;
   ```

### Phase 4: Rendering Frontend - Draw Character
**Goal**: Replace sphere rendering with mesh-based character visualization

**Rendering Changes** (in `app_bridge.c`, ~line 883):

**Old Code** (sphere-based):
```c
plSphere tSphere = {
    .fRadius = 0.04f,
    .tCenter = ptFrame->atBodyPos[i]
};
gptDraw->add_3d_sphere_filled(ptAppData->pt3dDrawlist, tSphere, 0, 0,
    (plDrawSolidOptions){.uColor = PL_COLOR_32_RGBA(0.95f, 0.9f, 0.1f, 1.0f)});
```

**New Code** (mesh-based):
```c
if(ptAppData->bCharacterLoaded) {
    // For each joint, apply transformation and draw mesh
    for(uint32_t i = 0; i < ptAppData->tCharacter.uJointCount && i < ptFrame->uBodyCount; i++) {
        plMesh* ptMesh = ptAppData->tCharacter.atMeshes[i];
        if(ptMesh == NULL) continue;
        
        // Create transform matrix with joint position
        plMat4 tTransform = pl_identity_mat4();
        tTransform.col[3] = (plVec4){
            ptFrame->atBodyPos[i].x,
            ptFrame->atBodyPos[i].y,
            ptFrame->atBodyPos[i].z,
            1.0f
        };
        
        // Draw mesh with color
        plDrawSolidOptions tDrawOptions = {
            .uColor = PL_COLOR_32_RGBA(
                ptAppData->tCharacter.atJointColors[i].x,
                ptAppData->tCharacter.atJointColors[i].y,
                ptAppData->tCharacter.atJointColors[i].z,
                ptAppData->tCharacter.atJointColors[i].w
            )
        };
        gptDraw->add_3d_mesh_filled(ptAppData->pt3dDrawlist, ptMesh, tTransform, 
                                     tDrawOptions);
    }
} else {
    // Fallback: render spheres as before
    // ... original sphere code ...
}
```

**Optional Enhancement**: Draw skeletal connections as tubes/cylinders:
```c
// Draw bones between joints using capsules
for(uint32_t i = 1; i < ptFrame->uBodyCount; i++) {
    plCapsule tCapsule = {
        .tPointA = ptFrame->atBodyPos[parent_idx[i]],
        .tPointB = ptFrame->atBodyPos[i],
        .fRadius = 0.015f
    };
    gptDraw->add_3d_capsule_filled(ptAppData->pt3dDrawlist, tCapsule, 0, 0,
        (plDrawSolidOptions){.uColor = PL_COLOR_32_RGBA(0.2f, 0.7f, 1.0f, 0.7f)});
}
```

### Phase 5: Integration & Initialization
**Goal**: Wire up character loading into the app startup flow

**Steps**:
1. **Modify `pl__app_load()` function** (in `app_bridge.c`):
   - After graphics initialization, call:
     ```c
     const char* acCharacterPath = "CLoSD/closd/data/assets/characters/default_humanoid/";
     ptAppData->tCharacter = pl__load_character_model(acCharacterPath);
     ptAppData->bCharacterLoaded = true;
     ```

2. **Add UI Toggle** (HUD in app_bridge.c):
   - Add boolean flag: `bShowCharacterMesh`
   - Add HUD button to toggle between sphere/mesh rendering
   - Display current character name in HUD

3. **Character Selection** (optional, Phase 2+):
   - Store multiple character configs
   - Allow runtime switching via control socket or HUD
   - Maintain character cache for quick switching

3. **Character Selection** (optional, Phase 2+):
   - Store multiple character configs
   - Allow runtime switching via control socket or HUD
   - Maintain character cache for quick switching

---

## Character Preparation Workflow

### Step-by-Step: Preparing Your Custom Character

**If you're using Option B (Custom Full-Body Mesh)**:

1. **In Blender (or your 3D software)**:
   - Import your humanoid character model
   - Ensure skeleton has exactly **24 bones** with **SMPL-compatible names**
   - Verify bones are properly weighted to mesh (skin weight > 0 for each vertex)
   - Orient model: Z-up, T-pose or A-pose (arms extended)
   - Scale to human proportions: ~1.7m height

2. **Export from Blender**:
   ```bash
   # Export to FBX with skeleton
   File → Export → FBX (.fbx)
   # Check: ✓ Armature, ✓ Mesh, ✓ Animations (if available)
   ```
   - Save as: `my_character.fbx`

3. **Run Mesh Splitter Script** (provided or create your own):
   ```bash
   python scripts/split_mesh_by_bones.py \
     --input my_character.fbx \
     --output CLoSD/closd/data/assets/characters/my_humanoid/meshes/
   ```
   - Output: 24 STL files (one per joint)
   - Location: `meshes/Pelvis.stl`, `meshes/L_Hand.stl`, etc.

4. **Create Configuration**:
   ```json
   // CLoSD/closd/data/assets/characters/my_humanoid/config.json
   {
     "name": "my_humanoid",
     "source": "Custom Blender Model",
     "joints": [
       {"index": 0, "name": "Pelvis", "color": [0.8, 0.7, 0.5, 1.0]},
       {"index": 1, "name": "L_Hip", "color": [0.7, 0.7, 0.7, 1.0]},
       ...
     ],
     "scale": 1.0,
     "height_m": 1.7
   }
   ```

5. **Place Files**:
   ```
   CLoSD/closd/data/assets/characters/my_humanoid/
   ├── config.json
   ├── skeleton.xml
   └── meshes/
       ├── Pelvis.stl
       ├── L_Hip.stl
       ├── L_Knee.stl
       ├── L_Ankle.stl
       ├── L_Toe.stl
       ├── R_Hip.stl
       ├── R_Knee.stl
       ├── R_Ankle.stl
       ├── R_Toe.stl
       ├── Torso.stl
       ├── Spine.stl
       ├── Chest.stl
       ├── Neck.stl
       ├── Head.stl
       ├── L_Thorax.stl
       ├── L_Shoulder.stl
       ├── L_Elbow.stl
       ├── L_Wrist.stl
       ├── L_Hand.stl
       ├── R_Thorax.stl
       ├── R_Shoulder.stl
       ├── R_Elbow.stl
       ├── R_Wrist.stl
       └── R_Hand.stl
   ```

6. **Update app_bridge.c to load your character**:
   ```c
   // In pl__app_load():
   const char* acCharacterPath = "CLoSD/closd/data/assets/characters/my_humanoid/";
   ptAppData->tCharacter = pl__load_character_model(acCharacterPath);
   ```

### Bone Naming Reference (MUST match exactly)

Must use these exact names for all 24 bones:

| # | Name | # | Name |
|---|------|---|------|
| 0 | Pelvis | 12 | Neck |
| 1 | L_Hip | 13 | Head |
| 2 | L_Knee | 14 | L_Thorax |
| 3 | L_Ankle | 15 | L_Shoulder |
| 4 | L_Toe | 16 | L_Elbow |
| 5 | R_Hip | 17 | L_Wrist |
| 6 | R_Knee | 18 | L_Hand |
| 7 | R_Ankle | 19 | R_Thorax |
| 8 | R_Toe | 20 | R_Shoulder |
| 9 | Torso | 21 | R_Elbow |
| 10 | Spine | 22 | R_Wrist |
| 11 | Chest | 23 | R_Hand |

---
## Troubleshooting & Common Issues

### Problem: "Bone names don't match SMPL"
**Solution**: Rename all 24 bones in your skeleton to exact SMPL names (case-sensitive)
- Use Blender's bone rename tool
- Verify with: `armature.bones.keys()` in Python console

### Problem: "Wrong mesh size/scale"
**Symptoms**: Character appears tiny or huge compared to joint positions
**Solution**: 
- Check mesh units (should be meters, not centimeters)
- Scale mesh to ~1.7m height
- In Blender: Scale bone positions to match, not just mesh

### Problem: "Deformed/twisted character"
**Symptoms**: Character appears warped or rotated incorrectly
**Solution**:
- Verify T-pose or A-pose (arms extended)
- Check coordinate system: Z-up (not Y-up)
- Ensure root bone (Pelvis) is at origin

### Problem: "STL files are empty"
**Symptoms**: Character visible at startup but not moving with joints
**Solution**:
- Verify skin weights are painted on mesh
- Each vertex must have weight > 0 for at least one bone
- Use Blender: Weight Paint mode to visualize

### Problem: "Can't load character on startup"
**Symptoms**: App crashes or falls back to spheres
**Solution**:
- Check file paths: must be exact, case-sensitive
- Verify config.json is valid JSON (use JSON validator)
- Ensure all 24 STL files exist in meshes/ folder
- Check app_bridge.c character path string matches directory name

---
### Phase 6: Optional Enhancements

1. **Rigging & Bone Transformation**:
   - Instead of position-only (current), extract quaternion rotations from bridge
   - Apply per-joint rotation to geometry
   - Requires extending BridgeFrame with rotation data

2. **Skinned Mesh Rendering**:
   - Instead of per-joint meshes, use single mesh with skinning weights
   - More efficient, better visual quality
   - More complex: requires skinning matrix setup in shader

3. **Character Animation States**:
   - Color code joints by activity (moving vs static)
   - Visualize joint velocities with mesh scaling/glow effects
   - Show joint limits as semi-transparent zones

4. **Multi-Character Support**:
   - Store multiple character models
   - Switch between humanoid, quadruped, other morphologies
   - Support parallel rendering of multiple agents

---

## Helper Scripts

### Blender Script: Split Mesh by Bones to STL

Save as `scripts/blender_split_mesh.py` and run in Blender:

```python
#!/usr/bin/env python3
"""
Blender script to split a rigged character mesh into per-bone STL files.
Run in Blender: blender --background your_character.blend --python scripts/blender_split_mesh.py
"""

import bpy
import bmesh
import os
import numpy as np
from pathlib import Path

def get_mesh_for_bone(obj, armature, bone_name, output_dir):
    """Extract mesh vertices weighted to a specific bone and save as STL"""
    
    if not armature or not obj.data.shape:
        print(f"  ⚠ No armature or mesh found")
        return False
    
    # Get vertex group index for this bone
    vgroup_index = None
    for i, vg in enumerate(obj.vertex_groups):
        if vg.name == bone_name:
            vgroup_index = i
            break
    
    if vgroup_index is None:
        print(f"  ⚠ No vertex group for bone: {bone_name}")
        return False
    
    # Create temporary mesh and select weighted vertices
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    
    verts_to_keep = []
    for vert in bm.verts:
        for group in vert.groups:
            if group.group == vgroup_index and group.weight > 0.1:
                verts_to_keep.append(vert)
                break
    
    if not verts_to_keep:
        print(f"  ⚠ No vertices for bone: {bone_name}")
        bm.free()
        return False
    
    # Delete unwanted vertices
    for vert in bm.verts:
        if vert not in verts_to_keep:
            bm.verts.remove(vert)
    
    # Clean up topology
    bmesh.ops.delete(bm, geom=[e for e in bm.edges if len(e.link_faces) == 0], context='EDGES')
    
    # Create mesh from bmesh
    mesh_temp = bpy.data.meshes.new(f"temp_{bone_name}")
    bm.to_mesh(mesh_temp)
    bm.free()
    
    # Save as STL
    os.makedirs(output_dir, exist_ok=True)
    stl_path = os.path.join(output_dir, f"{bone_name}.stl")
    
    # Export using Blender's STL exporter
    bpy.ops.object.select_all(action='DESELECT')
    mesh_obj = bpy.data.objects.new(f"obj_{bone_name}", mesh_temp)
    bpy.context.collection.objects.link(mesh_obj)
    mesh_obj.select_set(True)
    bpy.context.view_layer.objects.active = mesh_obj
    
    bpy.ops.export_mesh.stl(filepath=stl_path, use_selection=True)
    
    # Cleanup
    bpy.data.objects.remove(mesh_obj)
    bpy.data.meshes.remove(mesh_temp)
    
    print(f"  ✓ Saved: {stl_path}")
    return True

def main():
    # Configuration
    BONE_NAMES = [
        "Pelvis", "L_Hip", "L_Knee", "L_Ankle", "L_Toe", 
        "R_Hip", "R_Knee", "R_Ankle", "R_Toe",
        "Torso", "Spine", "Chest", "Neck", "Head",
        "L_Thorax", "L_Shoulder", "L_Elbow", "L_Wrist", "L_Hand",
        "R_Thorax", "R_Shoulder", "R_Elbow", "R_Wrist", "R_Hand"
    ]
    
    output_dir = "character_meshes"
    
    # Find mesh object
    mesh_obj = None
    armature_obj = None
    
    for obj in bpy.data.objects:
        if obj.type == 'MESH' and not obj.name.startswith("temp_"):
            mesh_obj = obj
            if obj.parent and obj.parent.type == 'ARMATURE':
                armature_obj = obj.parent
            break
    
    if not mesh_obj or not armature_obj:
        print("❌ Error: No rigged character found!")
        print("   Make sure you have a mesh with parent armature")
        return
    
    print(f"✓ Found mesh: {mesh_obj.name}")
    print(f"✓ Found armature: {armature_obj.name}")
    print(f"✓ Splitting into {len(BONE_NAMES)} bones...")
    print(f"✓ Output directory: {output_dir}")
    print()
    
    success_count = 0
    for bone_name in BONE_NAMES:
        print(f"Processing {bone_name}...")
        if get_mesh_for_bone(mesh_obj, armature_obj, bone_name, output_dir):
            success_count += 1
    
    print()
    print(f"✅ Complete! {success_count}/{len(BONE_NAMES)} bones exported")

if __name__ == "__main__":
    main()
```

**Usage**:
```bash
# Run Blender with script
blender --background my_character.blend --python scripts/blender_split_mesh.py

# Output: character_meshes/ with 24 STL files
ls character_meshes/
# Pelvis.stl, L_Hip.stl, L_Knee.stl, ...
```

### Python Script: Generate config.json

Save as `scripts/generate_character_config.py`:

```python
#!/usr/bin/env python3
"""Generate config.json for a character directory"""

import json
import os
from pathlib import Path

def generate_config(character_name, mesh_dir, output_path):
    """Create character configuration"""
    
    BONE_NAMES = [
        "Pelvis", "L_Hip", "L_Knee", "L_Ankle", "L_Toe", 
        "R_Hip", "R_Knee", "R_Ankle", "R_Toe",
        "Torso", "Spine", "Chest", "Neck", "Head",
        "L_Thorax", "L_Shoulder", "L_Elbow", "L_Wrist", "L_Hand",
        "R_Thorax", "R_Shoulder", "R_Elbow", "R_Wrist", "R_Hand"
    ]
    
    # Assign colors: blues for right side, greens for left, neutral for center
    colors = {}
    for i, bone in enumerate(BONE_NAMES):
        if bone.startswith("R_"):
            colors[bone] = [0.2, 0.5, 0.8, 1.0]  # Blue
        elif bone.startswith("L_"):
            colors[bone] = [0.3, 0.7, 0.3, 1.0]  # Green
        else:
            colors[bone] = [0.7, 0.7, 0.7, 1.0]  # Gray
    
    config = {
        "name": character_name,
        "source": "Custom Character",
        "description": f"Humanoid character with {len(BONE_NAMES)} bones",
        "joints": [
            {
                "index": i,
                "name": bone,
                "color": colors[bone],
                "mesh_file": f"meshes/{bone}.stl"
            }
            for i, bone in enumerate(BONE_NAMES)
        ],
        "scale": 1.0,
        "height_m": 1.7,
        "rigged": True,
        "coordinate_system": "Z-up"
    }
    
    with open(output_path, 'w') as f:
        json.dump(config, f, indent=2)
    
    print(f"✓ Generated: {output_path}")

if __name__ == "__main__":
    import sys
    
    if len(sys.argv) < 2:
        print("Usage: python generate_character_config.py <character_name> [output_dir]")
        print("Example: python generate_character_config.py my_humanoid ./config")
        sys.exit(1)
    
    character_name = sys.argv[1]
    output_dir = sys.argv[2] if len(sys.argv) > 2 else "."
    output_path = os.path.join(output_dir, "config.json")
    
    generate_config(character_name, "meshes", output_path)
```

**Usage**:
```bash
python scripts/generate_character_config.py my_humanoid CLoSD/closd/data/assets/characters/my_humanoid/

# Output: my_humanoid/config.json with all joint definitions
```

---

## File Structure Summary

### Files to Create/Modify

| Path | Type | Purpose |
|------|------|---------|
| `CLoSD/closd/data/assets/characters/default_humanoid/config.json` | NEW | Character metadata |
| `CLoSD/closd/data/assets/characters/default_humanoid/skeleton.xml` | NEW | MJCF skeleton structure |
| `CLoSD/closd/data/assets/characters/default_humanoid/meshes/` | NEW | STL files per joint |
| `pilotlight_integration/pilotlight_app/src/app_bridge.c` | MODIFY | Add mesh loading/rendering |
| `CLoSD/closd/<bridge_module>.py` | MODIFY | Send character metadata |

### Existing Assets Used

| Path | Description |
|------|-------------|
| `CLoSD/closd/data/assets/mjcf/smpl_humanoid.xml` | Reference skeleton structure |
| `CLoSD/closd/data/assets/mesh/smpl/` | Generated mesh cache |
| `CLoSD/closd/utils/smpllib/` | SMPL robot & mesh generation |

---

## Implementation Priority

1. **Must-have** (Phase 1-2): Data preparation + basic mesh rendering
2. **Should-have** (Phase 3-4): Full integration with fallback
3. **Nice-to-have** (Phase 5-6): Enhancements & multi-character

## Notes

- **Coordinate System**: PilotLight uses Z-up (matches CLoSD/Isaac convention)
- **Scale**: SMPL dimensions in meters; verify mesh scales match joint positions
- **Performance**: Mesh rendering faster than sphere computation; no negative impact
- **Backward Compatibility**: Keep sphere rendering as fallback when character not loaded
- **Asset Pipeline**: Consider automating mesh generation on startup or first launch

