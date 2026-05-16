/*
   app_bridge.c
     - PilotLight app plugin that visualizes CLoSD bridge packets
*/

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "pl.h"
#define PL_MATH_INCLUDE_FUNCTIONS
#include "pl_math.h"
#define PL_JSON_IMPLEMENTATION
#include "pl_json.h"

#include "pl_graphics_ext.h"
#include "pl_draw_ext.h"
#include "pl_starter_ext.h"

#define BRIDGE_HOST "127.0.0.1"
#define BRIDGE_PORT 45678
#define BRIDGE_CONTROL_HOST "127.0.0.1"
#define BRIDGE_CONTROL_PORT 45679
#define BRIDGE_BUFFER_SIZE 65535
#define MAX_BODIES 256
#define MAX_PRED_POINTS 256
#define MAX_DEBUG_SEGMENTS 512
#define MAX_PROMPT_CHARS 256

#define ARRAY_COUNT(x) (sizeof(x) / sizeof((x)[0]))

typedef struct _plCamera
{
    plVec3 tPos;
    float  fNearZ;
    float  fFarZ;
    float  fFieldOfView;
    float  fAspectRatio;
    plMat4 tViewMat;
    plMat4 tProjMat;
    plMat4 tTransformMat;

    float fPitch;
    float fYaw;
    float fRoll;

    plVec3 _tUpVec;
    plVec3 _tForwardVec;
    plVec3 _tRightVec;
} plCamera;

typedef struct _BridgeFrame
{
    uint64_t uSeq;
    int32_t iEnvId;
    float fDt;

    uint32_t uBodyCount;
    plVec3   atBodyPos[MAX_BODIES];

    uint32_t uPredCount;
    plVec3   atPredPos[MAX_PRED_POINTS];

    uint32_t uDebugSegmentCount;
    plVec3   atDebugSegStart[MAX_DEBUG_SEGMENTS];
    plVec3   atDebugSegEnd[MAX_DEBUG_SEGMENTS];

    bool     bHasSpawnAnchor;
    plVec3   tSpawnAnchor;
    bool     bHasWorldCenter;
    plVec3   tWorldCenter;
    bool     bSpawnFollowLast;

    char acPrompt[MAX_PROMPT_CHARS];
} BridgeFrame;

typedef struct _plAppData
{
    plWindow* ptWindow;
    plDrawList3D* pt3dDrawlist;
    plDrawList2D* ptHudDrawlist;
    plDrawLayer2D* ptHudLayer;
    plCamera tCamera;

    int iSocketFd;
    int iControlSocketFd;
    struct sockaddr_in tControlAddr;
    bool bHasFrame;
    bool bAutoFollow;
    bool bFollowLastSpawn;

    // orbit camera
    plVec3 tOrbitTarget;
    float  fOrbitDist;
    bool   bCameraInitializedFromFrame;
    BridgeFrame tFrame;
    char acBuffer[BRIDGE_BUFFER_SIZE + 1];

    uint64_t uPacketsParsed;
    uint64_t uPacketParseFailures;
    double dLastStatsPrintTime;

    bool bShowYellow;
    bool bShowBlue;
    bool bShowGreen;
    bool bShowRed;
    bool bHudWantsMouse;
} plAppData;

const plIOI*       gptIO      = NULL;
const plWindowI*   gptWindows = NULL;
const plGraphicsI* gptGfx     = NULL;
const plDrawI*     gptDraw    = NULL;
const plStarterI*  gptStarter = NULL;

// Shared HUD palette values for quick tweaking and consistency.
static const plVec3 gtUiColorToggleOn  = {0.20f, 0.45f, 0.20f};
static const plVec3 gtUiColorToggleOff = {0.25f, 0.25f, 0.25f};

static inline uint32_t
pl__ui_color_rgb_alpha(plVec3 tRgb, float fAlpha)
{
    return PL_COLOR_32_RGBA(tRgb.x, tRgb.y, tRgb.z, fAlpha);
}

static inline uint32_t
pl__ui_color_toggle_bg(bool bEnabled, bool bHover)
{
    if(bEnabled)
        return pl__ui_color_rgb_alpha(gtUiColorToggleOn, bHover ? 0.95f : 0.85f);
    return pl__ui_color_rgb_alpha(gtUiColorToggleOff, bHover ? 0.85f : 0.75f);
}

static inline float
pl__wrap_angle(float tTheta)
{
    static const float f2Pi = 2.0f * PL_PI;
    const float fMod = fmodf(tTheta, f2Pi);
    if (fMod > PL_PI)
        return fMod - f2Pi;
    else if (fMod < -PL_PI)
        return fMod + f2Pi;
    return fMod;
}

static void
pl__camera_update(plCamera* ptCamera)
{
    // Z-up camera basis to match CLoSD/Isaac coordinates.
    const plVec3 tWorldUp = {0.0f, 0.0f, 1.0f};

    // Forward from yaw (around Z) and pitch.
    const plVec3 tForward = pl_norm_vec3((plVec3){
        cosf(ptCamera->fPitch) * cosf(ptCamera->fYaw),
        cosf(ptCamera->fPitch) * sinf(ptCamera->fYaw),
        sinf(ptCamera->fPitch)
    });

    plVec3 tRight = pl_cross_vec3(tForward, tWorldUp);
    if(pl_length_vec3(tRight) < 1e-5f)
        tRight = (plVec3){1.0f, 0.0f, 0.0f};
    else
        tRight = pl_norm_vec3(tRight);

    plVec3 tUp = pl_norm_vec3(pl_cross_vec3(tRight, tForward));

    // Keep camera upright in Z-up world: if up points downward, flip roll by 180 deg.
    if(pl_dot_vec3(tUp, tWorldUp) < 0.0f)
    {
        tRight = pl_mul_vec3_scalarf(tRight, -1.0f);
        tUp = pl_mul_vec3_scalarf(tUp, -1.0f);
    }

    ptCamera->_tForwardVec = tForward;
    ptCamera->_tRightVec   = tRight;
    ptCamera->_tUpVec      = tUp;

    ptCamera->tTransformMat = (plMat4){
        .col = {
            {tRight.x,   tRight.y,   tRight.z,   0.0f},
            {tUp.x,      tUp.y,      tUp.z,      0.0f},
            {tForward.x, tForward.y, tForward.z, 0.0f},
            {ptCamera->tPos.x, ptCamera->tPos.y, ptCamera->tPos.z, 1.0f}
        }
    };
    ptCamera->tViewMat = pl_mat4t_invert(&ptCamera->tTransformMat);

    const float fInvtanHalfFovy = 1.0f / tanf(ptCamera->fFieldOfView / 2.0f);
    ptCamera->tProjMat = pl_identity_mat4();
    ptCamera->tProjMat.col[0].x = fInvtanHalfFovy / ptCamera->fAspectRatio;
    // Flip Y for this renderer path so the world doesn't start upside down.
    ptCamera->tProjMat.col[1].y = -fInvtanHalfFovy;
    ptCamera->tProjMat.col[2].z = ptCamera->fFarZ / (ptCamera->fFarZ - ptCamera->fNearZ);
    ptCamera->tProjMat.col[2].w = 1.0f;
    ptCamera->tProjMat.col[3].z = -ptCamera->fNearZ * ptCamera->fFarZ / (ptCamera->fFarZ - ptCamera->fNearZ);
    ptCamera->tProjMat.col[3].w = 0.0f;
}

// Reposition the camera to orbit around tTarget at distance fDist using current pitch/yaw.
static void
pl__camera_orbit_from_angles(plCamera* ptCamera, plVec3 tTarget, float fDist)
{
    // Z-up spherical orbit: yaw rotates around +Z axis, pitch tilts up/down.
    const float fFx = cosf(ptCamera->fPitch) * cosf(ptCamera->fYaw);
    const float fFy = cosf(ptCamera->fPitch) * sinf(ptCamera->fYaw);
    const float fFz = sinf(ptCamera->fPitch);
    ptCamera->tPos.x = tTarget.x - fFx * fDist;
    ptCamera->tPos.y = tTarget.y - fFy * fDist;
    ptCamera->tPos.z = tTarget.z - fFz * fDist;
    pl__camera_update(ptCamera);
}

static bool
pl__project_world_to_screen(const plCamera* ptCamera, plVec2 tViewport, plVec3 tWorld, plVec2* ptOut)
{
    if(!ptCamera || !ptOut || tViewport.x <= 0.0f || tViewport.y <= 0.0f)
        return false;

    const plMat4 tMVP = pl_mul_mat4((plMat4*)&ptCamera->tProjMat, (plMat4*)&ptCamera->tViewMat);
    const float fX = tWorld.x;
    const float fY = tWorld.y;
    const float fZ = tWorld.z;

    const float fClipX = tMVP.col[0].x * fX + tMVP.col[1].x * fY + tMVP.col[2].x * fZ + tMVP.col[3].x;
    const float fClipY = tMVP.col[0].y * fX + tMVP.col[1].y * fY + tMVP.col[2].y * fZ + tMVP.col[3].y;
    const float fClipZ = tMVP.col[0].z * fX + tMVP.col[1].z * fY + tMVP.col[2].z * fZ + tMVP.col[3].z;
    const float fClipW = tMVP.col[0].w * fX + tMVP.col[1].w * fY + tMVP.col[2].w * fZ + tMVP.col[3].w;

    if(fabsf(fClipW) < 1e-5f)
        return false;

    const float fInvW = 1.0f / fClipW;
    const float fNdcX = fClipX * fInvW;
    const float fNdcY = fClipY * fInvW;
    const float fNdcZ = fClipZ * fInvW;

    if(fNdcZ < 0.0f || fNdcZ > 1.0f)
        return false;

    ptOut->x = (fNdcX * 0.5f + 0.5f) * tViewport.x;
    ptOut->y = (fNdcY * 0.5f + 0.5f) * tViewport.y;
    return true;
}

static bool
pl__measure_pixels_for_meter(const plAppData* ptAppData, plVec2 tViewport, float fGroundZ, float* pfOutPixels)
{
    if(!ptAppData || !pfOutPixels)
        return false;

    const float fMeter = 1.0f;
    const plVec3 tP0 = {
        ptAppData->tOrbitTarget.x - 0.5f * fMeter,
        ptAppData->tOrbitTarget.y,
        fGroundZ + 0.02f,
    };
    const plVec3 tP1 = {
        ptAppData->tOrbitTarget.x + 0.5f * fMeter,
        ptAppData->tOrbitTarget.y,
        fGroundZ + 0.02f,
    };

    plVec2 tS0 = {0.0f, 0.0f};
    plVec2 tS1 = {0.0f, 0.0f};
    if(!pl__project_world_to_screen(&ptAppData->tCamera, tViewport, tP0, &tS0) ||
       !pl__project_world_to_screen(&ptAppData->tCamera, tViewport, tP1, &tS1))
        return false;

    const float fDx = tS1.x - tS0.x;
    const float fDy = tS1.y - tS0.y;
    const float fPixels = sqrtf(fDx * fDx + fDy * fDy);
    if(fPixels < 1e-3f)
        return false;

    *pfOutPixels = fPixels;
    return true;
}
static bool
pl__open_bridge_socket(plAppData* ptAppData)
{
    ptAppData->iSocketFd = socket(AF_INET, SOCK_DGRAM, 0);
    if(ptAppData->iSocketFd < 0)
        return false;

    int iFlags = fcntl(ptAppData->iSocketFd, F_GETFL, 0);
    if(iFlags >= 0)
        fcntl(ptAppData->iSocketFd, F_SETFL, iFlags | O_NONBLOCK);

    int iOpt = 1;
    setsockopt(ptAppData->iSocketFd, SOL_SOCKET, SO_REUSEADDR, &iOpt, sizeof(iOpt));

    struct sockaddr_in tAddr;
    memset(&tAddr, 0, sizeof(tAddr));
    tAddr.sin_family = AF_INET;
    tAddr.sin_port = htons(BRIDGE_PORT);
    tAddr.sin_addr.s_addr = inet_addr(BRIDGE_HOST);

    if(bind(ptAppData->iSocketFd, (struct sockaddr*)&tAddr, sizeof(tAddr)) < 0)
    {
        close(ptAppData->iSocketFd);
        ptAppData->iSocketFd = -1;
        return false;
    }

    return true;
}

static void
pl__close_bridge_socket(plAppData* ptAppData)
{
    if(ptAppData->iSocketFd >= 0)
    {
        close(ptAppData->iSocketFd);
        ptAppData->iSocketFd = -1;
    }
}

static bool
pl__open_control_socket(plAppData* ptAppData)
{
    ptAppData->iControlSocketFd = socket(AF_INET, SOCK_DGRAM, 0);
    if(ptAppData->iControlSocketFd < 0)
        return false;

    memset(&ptAppData->tControlAddr, 0, sizeof(ptAppData->tControlAddr));
    ptAppData->tControlAddr.sin_family = AF_INET;
    ptAppData->tControlAddr.sin_port = htons(BRIDGE_CONTROL_PORT);
    ptAppData->tControlAddr.sin_addr.s_addr = inet_addr(BRIDGE_CONTROL_HOST);
    return true;
}

static void
pl__close_control_socket(plAppData* ptAppData)
{
    if(ptAppData->iControlSocketFd >= 0)
    {
        close(ptAppData->iControlSocketFd);
        ptAppData->iControlSocketFd = -1;
    }
}

static void
pl__send_control_json(plAppData* ptAppData, const char* pcJson)
{
    if(!ptAppData || ptAppData->iControlSocketFd < 0 || !pcJson)
        return;

    sendto(
        ptAppData->iControlSocketFd,
        pcJson,
        strlen(pcJson),
        0,
        (const struct sockaddr*)&ptAppData->tControlAddr,
        sizeof(ptAppData->tControlAddr));
}

static void
pl__send_spawn_mode(plAppData* ptAppData)
{
    char acPayload[128] = {0};
    snprintf(
        acPayload,
        sizeof(acPayload),
        "{\"action\":\"set_spawn_mode\",\"follow_last\":%s}",
        ptAppData->bFollowLastSpawn ? "true" : "false");
    pl__send_control_json(ptAppData, acPayload);
}

static void
pl__send_capture_spawn_anchor(plAppData* ptAppData)
{
    const int iEnv = ptAppData->bHasFrame ? ptAppData->tFrame.iEnvId : 0;
    char acPayload[128] = {0};
    snprintf(
        acPayload,
        sizeof(acPayload),
        "{\"action\":\"capture_spawn_anchor\",\"env_id\":%d}",
        iEnv);
    pl__send_control_json(ptAppData, acPayload);
}

static void
pl__send_reset_episode(plAppData* ptAppData)
{
    pl__send_control_json(ptAppData, "{\"action\":\"reset_episode\"}");
}

static void
pl__parse_optional_vec3_member(plJsonObject* ptRoot, const char* pcName, plVec3* ptOut, bool* pbHas)
{
    if(pbHas)
        *pbHas = false;
    if(!ptRoot || !pcName || !ptOut)
        return;

    uint32_t uUnused = 0;
    plJsonObject* ptArr = pl_json_array_member(ptRoot, pcName, &uUnused);
    if(!ptArr)
        return;

    float af[3] = {0.0f, 0.0f, 0.0f};
    uint32_t uSize = 3;
    pl_json_as_float_array(ptArr, af, &uSize);
    if(uSize < 3)
        return;

    *ptOut = (plVec3){af[0], af[1], af[2]};
    if(pbHas)
        *pbHas = true;
}

static void
pl__parse_vec3_array(plJsonObject* ptArray, plVec3* atOut, uint32_t uMaxCount, uint32_t* puOutCount)
{
    *puOutCount = 0;
    if(!ptArray)
        return;

    // ptArray is already an array object, iterate by index.
    // We infer length by probing until NULL.
    for(uint32_t i = 0; i < uMaxCount; i++)
    {
        plJsonObject* ptVec = pl_json_member_by_index(ptArray, i);
        if(!ptVec)
            break;

        float af[3] = {0.0f, 0.0f, 0.0f};
        uint32_t uSize = 3;
        pl_json_as_float_array(ptVec, af, &uSize);
        if(uSize < 3)
            continue;

        atOut[*puOutCount] = (plVec3){af[0], af[1], af[2]};
        (*puOutCount)++;
    }
}

static void
pl__parse_debug_segments(plJsonObject* ptArray, BridgeFrame* ptFrame)
{
    ptFrame->uDebugSegmentCount = 0;
    if(!ptArray)
        return;

    for(uint32_t i = 0; i < MAX_DEBUG_SEGMENTS; i++)
    {
        plJsonObject* ptSeg = pl_json_member_by_index(ptArray, i);
        if(!ptSeg)
            break;

        float af[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        uint32_t uSize = 6;
        pl_json_as_float_array(ptSeg, af, &uSize);
        if(uSize < 6)
            continue;

        ptFrame->atDebugSegStart[ptFrame->uDebugSegmentCount] = (plVec3){af[0], af[1], af[2]};
        ptFrame->atDebugSegEnd[ptFrame->uDebugSegmentCount] = (plVec3){af[3], af[4], af[5]};
        ptFrame->uDebugSegmentCount++;
    }
}

static bool
pl__parse_bridge_packet(const char* pcJson, BridgeFrame* ptFrame)
{
    plJsonObject* ptRoot = NULL;
    if(!pl_load_json(pcJson, &ptRoot) || !ptRoot)
        return false;

    BridgeFrame tTmp = {0};
    tTmp.uSeq = pl_json_uint_member(ptRoot, "seq", 0);
    tTmp.iEnvId = pl_json_int_member(ptRoot, "env_id", 0);
    tTmp.fDt = pl_json_float_member(ptRoot, "dt", 1.0f / 60.0f);

    plJsonObject* ptRigidBodies = pl_json_member(ptRoot, "rigid_bodies");
    if(ptRigidBodies)
    {
        uint32_t uUnused = 0;
        plJsonObject* ptPos = pl_json_array_member(ptRigidBodies, "pos", &uUnused);
        pl__parse_vec3_array(ptPos, tTmp.atBodyPos, MAX_BODIES, &tTmp.uBodyCount);
    }

    plJsonObject* ptPred = pl_json_member(ptRoot, "predicted");
    if(ptPred && pl_json_bool_member(ptPred, "enabled", false))
    {
        uint32_t uUnused = 0;
        plJsonObject* ptPoses = pl_json_array_member(ptPred, "poses", &uUnused);
        pl__parse_vec3_array(ptPoses, tTmp.atPredPos, MAX_PRED_POINTS, &tTmp.uPredCount);
    }

    plJsonObject* ptDebug = pl_json_member(ptRoot, "debug");
    if(ptDebug)
    {
        uint32_t uUnused = 0;
        plJsonObject* ptSegments = pl_json_array_member(ptDebug, "segments", &uUnused);
        pl__parse_debug_segments(ptSegments, &tTmp);
    }

    tTmp.bHasSpawnAnchor = false;
    tTmp.bHasWorldCenter = false;
    tTmp.bSpawnFollowLast = pl_json_bool_member(ptRoot, "spawn_follow_last", false);
    pl__parse_optional_vec3_member(ptRoot, "spawn_anchor", &tTmp.tSpawnAnchor, &tTmp.bHasSpawnAnchor);
    pl__parse_optional_vec3_member(ptRoot, "world_center", &tTmp.tWorldCenter, &tTmp.bHasWorldCenter);

    // Optional prompt text from the sender.
    tTmp.acPrompt[0] = '\0';
    if(!pl_json_string_member(ptRoot, "text_prompt", tTmp.acPrompt, MAX_PROMPT_CHARS))
        pl_json_string_member(ptRoot, "prompt", tTmp.acPrompt, MAX_PROMPT_CHARS);

    pl_unload_json(&ptRoot);
    *ptFrame = tTmp;
    return true;
}

static void
pl__poll_bridge_socket(plAppData* ptAppData)
{
    if(ptAppData->iSocketFd < 0)
        return;

    while(true)
    {
        const int iRead = (int)recvfrom(ptAppData->iSocketFd, ptAppData->acBuffer, BRIDGE_BUFFER_SIZE, 0, NULL, NULL);
        if(iRead <= 0)
        {
            if(errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            break;
        }

        ptAppData->acBuffer[iRead] = '\0';
        BridgeFrame tFrame = {0};
        if(pl__parse_bridge_packet(ptAppData->acBuffer, &tFrame))
        {
            ptAppData->tFrame = tFrame;
            ptAppData->bHasFrame = true;
            ptAppData->bFollowLastSpawn = tFrame.bSpawnFollowLast;
            ptAppData->uPacketsParsed++;

            if(ptAppData->uPacketsParsed <= 3 || (ptAppData->uPacketsParsed % 120ull) == 0ull)
            {
                const plVec3 tRoot = tFrame.uBodyCount > 0 ? tFrame.atBodyPos[0] : (plVec3){0.0f, 0.0f, 0.0f};
                printf("[bridge] pkt=%llu env=%d bodies=%u pred=%u dbg=%u root=(%.3f, %.3f, %.3f)\n",
                    (unsigned long long)tFrame.uSeq,
                    tFrame.iEnvId,
                    tFrame.uBodyCount,
                    tFrame.uPredCount,
                    tFrame.uDebugSegmentCount,
                    tRoot.x, tRoot.y, tRoot.z);
            }
        }
        else
            ptAppData->uPacketParseFailures++;
    }
}

static float
pl__estimate_ground_z(const BridgeFrame* ptFrame)
{
    if(!ptFrame || ptFrame->uBodyCount == 0)
        return 0.0f;

    float fMinZ = ptFrame->atBodyPos[0].z;
    for(uint32_t i = 1; i < ptFrame->uBodyCount; i++)
    {
        if(ptFrame->atBodyPos[i].z < fMinZ)
            fMinZ = ptFrame->atBodyPos[i].z;
    }
    return fMinZ;
}

static inline plVec3
pl__ref_to_sim_vec3(plVec3 tRef)
{
    // Convert CLoSD ref-like coordinates [x, -z, y] into viewer sim coordinates [x, y, z].
    return (plVec3){tRef.x, tRef.z, -tRef.y};
}

static inline float
pl__dist_sq_vec3(plVec3 a, plVec3 b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

static void
pl__process_hud_buttons(plAppData* ptAppData)
{
    if(!ptAppData)
        return;

    ptAppData->bHudWantsMouse = false;

    const float fButtonX = 20.0f;
    const float fButtonY = 260.0f;
    const float fButtonW = 92.0f;
    const float fButtonH = 22.0f;
    const float fGap = 8.0f;

    bool* apbToggles[4] = {
        &ptAppData->bShowGreen,
        &ptAppData->bShowYellow,
        &ptAppData->bShowBlue,
        &ptAppData->bShowRed,
    };

    for(uint32_t i = 0; i < 4; i++)
    {
        const float fX0 = fButtonX + (fButtonW + fGap) * (float)i;
        const plVec2 tMin = {fX0, fButtonY};
        const plVec2 tMax = {fX0 + fButtonW, fButtonY + fButtonH};
        const bool bHover = gptIO->is_mouse_hovering_rect(tMin, tMax);
        if(bHover)
            ptAppData->bHudWantsMouse = true;

        if(bHover && gptIO->is_mouse_clicked(PL_MOUSE_BUTTON_LEFT, false))
            *apbToggles[i] = !(*apbToggles[i]);
    }

    const plVec2 tFollowMin = {20.0f, 304.0f};
    const plVec2 tFollowMax = {128.0f, 328.0f};
    const bool bFollowHover = gptIO->is_mouse_hovering_rect(tFollowMin, tFollowMax);
    if(bFollowHover)
        ptAppData->bHudWantsMouse = true;
    if(bFollowHover && gptIO->is_mouse_clicked(PL_MOUSE_BUTTON_LEFT, false))
    {
        ptAppData->bFollowLastSpawn = !ptAppData->bFollowLastSpawn;
        pl__send_spawn_mode(ptAppData);
    }

    const plVec2 tCaptureMin = {136.0f, 304.0f};
    const plVec2 tCaptureMax = {252.0f, 328.0f};
    const bool bCaptureHover = gptIO->is_mouse_hovering_rect(tCaptureMin, tCaptureMax);
    if(bCaptureHover)
        ptAppData->bHudWantsMouse = true;
    if(bCaptureHover && gptIO->is_mouse_clicked(PL_MOUSE_BUTTON_LEFT, false))
        pl__send_capture_spawn_anchor(ptAppData);

    const plVec2 tResetMin = {260.0f, 304.0f};
    const plVec2 tResetMax = {350.0f, 328.0f};
    const bool bResetHover = gptIO->is_mouse_hovering_rect(tResetMin, tResetMax);
    if(bResetHover)
        ptAppData->bHudWantsMouse = true;
    if(bResetHover && gptIO->is_mouse_clicked(PL_MOUSE_BUTTON_LEFT, false))
        pl__send_reset_episode(ptAppData);
}

static void
pl__draw_ground_grid(plAppData* ptAppData, plVec3 tCenter, float fGroundZ)
{
    const int   iHalf = 24;
    const float fLine = 0.06f;
    const plDrawLineOptions tGrid  = {.uColor = PL_COLOR_32_RGBA(0.30f, 0.30f, 0.30f, 1.0f), .fThickness = fLine};
    const plDrawLineOptions tAxisX = {.uColor = PL_COLOR_32_RGBA(0.75f, 0.25f, 0.25f, 1.0f), .fThickness = fLine * 2.0f};
    const plDrawLineOptions tAxisY = {.uColor = PL_COLOR_32_RGBA(0.25f, 0.75f, 0.25f, 1.0f), .fThickness = fLine * 2.0f};

    // XY grid at fixed Z (CLoSD data is Z-up).
    for(int i = -iHalf; i <= iHalf; i++)
    {
        const float fX = tCenter.x + (float)i;
        const float fY = tCenter.y + (float)i;
        const plDrawLineOptions tOptX = (i == 0) ? tAxisY : tGrid;
        const plDrawLineOptions tOptY = (i == 0) ? tAxisX : tGrid;

        // Lines parallel to +Y/-Y (vary X).
        gptDraw->add_3d_line(ptAppData->pt3dDrawlist,
            (plVec3){fX, tCenter.y - (float)iHalf, fGroundZ},
            (plVec3){fX, tCenter.y + (float)iHalf, fGroundZ},
            tOptX);

        // Lines parallel to +X/-X (vary Y).
        gptDraw->add_3d_line(ptAppData->pt3dDrawlist,
            (plVec3){tCenter.x - (float)iHalf, fY, fGroundZ},
            (plVec3){tCenter.x + (float)iHalf, fY, fGroundZ},
            tOptY);
    }
}

static void
pl__draw_hud(plAppData* ptAppData, float fGroundZ)
{
    if(!ptAppData || !ptAppData->ptHudLayer)
        return;

    const char* pcPrompt = "(no prompt in packet)";
    if(ptAppData->bHasFrame && ptAppData->tFrame.acPrompt[0] != '\0')
        pcPrompt = ptAppData->tFrame.acPrompt;

    gptDraw->add_rect_filled(
        ptAppData->ptHudLayer,
        (plVec2){12.0f, 12.0f},
        (plVec2){760.0f, 58.0f},
        (plDrawSolidOptions){.uColor = PL_COLOR_32_RGBA(0.05f, 0.05f, 0.05f, 0.80f)});

    gptDraw->add_text(
        ptAppData->ptHudLayer,
        (plVec2){20.0f, 26.0f},
        "Prompt:",
        (plDrawTextOptions){.ptFont = gptStarter->get_default_font(), .uColor = PL_COLOR_32_RGBA(1.0f, 0.85f, 0.2f, 1.0f), .fSize = 15.0f});

    gptDraw->add_text(
        ptAppData->ptHudLayer,
        (plVec2){92.0f, 26.0f},
        pcPrompt,
        (plDrawTextOptions){.ptFont = gptStarter->get_default_font(), .uColor = PL_COLOR_32_RGBA(1.0f, 1.0f, 1.0f, 1.0f), .fSize = 15.0f, .fWrap = 650.0f});

    // Camera panel (imgui-like info block)
    gptDraw->add_rect_filled(
        ptAppData->ptHudLayer,
        (plVec2){12.0f, 66.0f},
        (plVec2){460.0f, 340.0f},
        (plDrawSolidOptions){.uColor = PL_COLOR_32_RGBA(0.04f, 0.04f, 0.04f, 0.78f)});

    gptDraw->add_text(
        ptAppData->ptHudLayer,
        (plVec2){20.0f, 78.0f},
        "Camera",
        (plDrawTextOptions){.ptFont = gptStarter->get_default_font(), .uColor = PL_COLOR_32_RGBA(0.65f, 0.85f, 1.0f, 1.0f), .fSize = 14.0f});

    char acLine[256];
    float fYawDeg = ptAppData->tCamera.fYaw * (180.0f / PL_PI);
    float fPitchDeg = ptAppData->tCamera.fPitch * (180.0f / PL_PI);

    snprintf(acLine, sizeof(acLine), "pos    x %.2f  y %.2f  z %.2f", ptAppData->tCamera.tPos.x, ptAppData->tCamera.tPos.y, ptAppData->tCamera.tPos.z);
    gptDraw->add_text(ptAppData->ptHudLayer, (plVec2){20.0f, 98.0f}, acLine,
        (plDrawTextOptions){.ptFont = gptStarter->get_default_font(), .uColor = PL_COLOR_32_RGBA(1.0f, 1.0f, 1.0f, 1.0f), .fSize = 13.0f});

    snprintf(acLine, sizeof(acLine), "target x %.2f  y %.2f  z %.2f", ptAppData->tOrbitTarget.x, ptAppData->tOrbitTarget.y, ptAppData->tOrbitTarget.z);
    gptDraw->add_text(ptAppData->ptHudLayer, (plVec2){20.0f, 116.0f}, acLine,
        (plDrawTextOptions){.ptFont = gptStarter->get_default_font(), .uColor = PL_COLOR_32_RGBA(1.0f, 1.0f, 1.0f, 1.0f), .fSize = 13.0f});

    snprintf(acLine, sizeof(acLine), "yaw %.1f deg   pitch %.1f deg", fYawDeg, fPitchDeg);
    gptDraw->add_text(ptAppData->ptHudLayer, (plVec2){20.0f, 134.0f}, acLine,
        (plDrawTextOptions){.ptFont = gptStarter->get_default_font(), .uColor = PL_COLOR_32_RGBA(1.0f, 1.0f, 1.0f, 1.0f), .fSize = 13.0f});

    snprintf(acLine, sizeof(acLine), "distance %.2f   ground z %.2f", ptAppData->fOrbitDist, fGroundZ);
    gptDraw->add_text(ptAppData->ptHudLayer, (plVec2){20.0f, 152.0f}, acLine,
        (plDrawTextOptions){.ptFont = gptStarter->get_default_font(), .uColor = PL_COLOR_32_RGBA(1.0f, 1.0f, 1.0f, 1.0f), .fSize = 13.0f});

    snprintf(acLine, sizeof(acLine), "auto-follow %s   bodies %u", ptAppData->bAutoFollow ? "on" : "off", ptAppData->tFrame.uBodyCount);
    gptDraw->add_text(ptAppData->ptHudLayer, (plVec2){20.0f, 170.0f}, acLine,
        (plDrawTextOptions){.ptFont = gptStarter->get_default_font(), .uColor = PL_COLOR_32_RGBA(0.85f, 0.95f, 0.85f, 1.0f), .fSize = 13.0f});

    gptDraw->add_text(ptAppData->ptHudLayer, (plVec2){20.0f, 188.0f},
        "GREEN: Predicted trajectory/pose points from CLoSD",
        (plDrawTextOptions){.ptFont = gptStarter->get_default_font(), .uColor = PL_COLOR_32_RGBA(0.2f, 1.0f, 0.4f, 1.0f), .fSize = 12.0f});
    gptDraw->add_text(ptAppData->ptHudLayer, (plVec2){20.0f, 204.0f},
        "YELLOW: Live rigid-body joint points",
        (plDrawTextOptions){.ptFont = gptStarter->get_default_font(), .uColor = PL_COLOR_32_RGBA(0.95f, 0.9f, 0.1f, 1.0f), .fSize = 12.0f});
    gptDraw->add_text(ptAppData->ptHudLayer, (plVec2){20.0f, 220.0f},
        "BLUE: Live skeleton bone links",
        (plDrawTextOptions){.ptFont = gptStarter->get_default_font(), .uColor = PL_COLOR_32_RGBA(0.2f, 0.7f, 1.0f, 1.0f), .fSize = 12.0f});
    gptDraw->add_text(ptAppData->ptHudLayer, (plVec2){20.0f, 236.0f},
        "RED: Root/center marker",
        (plDrawTextOptions){.ptFont = gptStarter->get_default_font(), .uColor = PL_COLOR_32_RGBA(1.0f, 0.3f, 0.2f, 1.0f), .fSize = 12.0f});

    // Live metric ruler: shows how large 1 meter appears with current camera setup.
    plIO* ptIO = gptIO->get_io();
    const plVec2 tViewport = {ptIO->tMainViewportSize.x, ptIO->tMainViewportSize.y};
    float fPixelsPerMeter = 0.0f;
    if(pl__measure_pixels_for_meter(ptAppData, tViewport, fGroundZ, &fPixelsPerMeter))
    {
        const float fBarPx = pl_clampf(24.0f, fPixelsPerMeter, 180.0f);
        const plVec2 tA = {568.0f, 86.0f};
        const plVec2 tB = {568.0f + fBarPx, 86.0f};
        const plDrawLineOptions tScaleLine = {
            .uColor = PL_COLOR_32_RGBA(0.95f, 0.95f, 0.95f, 1.0f),
            .fThickness = 1.6f,
        };
        gptDraw->add_line(ptAppData->ptHudLayer, tA, tB, tScaleLine);
        gptDraw->add_line(ptAppData->ptHudLayer, (plVec2){tA.x, tA.y - 5.0f}, (plVec2){tA.x, tA.y + 5.0f}, tScaleLine);
        gptDraw->add_line(ptAppData->ptHudLayer, (plVec2){tB.x, tB.y - 5.0f}, (plVec2){tB.x, tB.y + 5.0f}, tScaleLine);

        char acScale[128] = {0};
        snprintf(acScale, sizeof(acScale), "scale |__| = 1.00 m (%.0f px)", fPixelsPerMeter);
        gptDraw->add_text(ptAppData->ptHudLayer, (plVec2){568.0f, 94.0f}, acScale,
            (plDrawTextOptions){.ptFont = gptStarter->get_default_font(), .uColor = PL_COLOR_32_RGBA(0.90f, 0.95f, 1.0f, 1.0f), .fSize = 12.0f});
    }
    else
    {
        gptDraw->add_text(ptAppData->ptHudLayer, (plVec2){568.0f, 94.0f}, "scale |__| = 1.00 m (off-screen)",
            (plDrawTextOptions){.ptFont = gptStarter->get_default_font(), .uColor = PL_COLOR_32_RGBA(0.70f, 0.75f, 0.80f, 1.0f), .fSize = 12.0f});
    }

    const float fButtonX = 20.0f;
    const float fButtonY = 260.0f;
    const float fButtonW = 92.0f;
    const float fButtonH = 22.0f;
    const float fGap = 8.0f;

    const char* apcLabels[4] = {"GREEN", "YELLOW", "BLUE", "RED"};
    const bool abEnabled[4] = {
        ptAppData->bShowGreen,
        ptAppData->bShowYellow,
        ptAppData->bShowBlue,
        ptAppData->bShowRed,
    };

    for(uint32_t i = 0; i < 4; i++)
    {
        const float fX0 = fButtonX + (fButtonW + fGap) * (float)i;
        const plVec2 tMin = {fX0, fButtonY};
        const plVec2 tMax = {fX0 + fButtonW, fButtonY + fButtonH};
        const bool bHover = gptIO->is_mouse_hovering_rect(tMin, tMax);
        const plDrawSolidOptions tBg = {
            .uColor = pl__ui_color_toggle_bg(abEnabled[i], bHover)
        };
        gptDraw->add_rect_filled(ptAppData->ptHudLayer, tMin, tMax, tBg);

        char acButton[32] = {0};
        snprintf(acButton, sizeof(acButton), "%s %s", apcLabels[i], abEnabled[i] ? "ON" : "OFF");
        gptDraw->add_text(ptAppData->ptHudLayer, (plVec2){fX0 + 6.0f, fButtonY + 5.0f}, acButton,
            (plDrawTextOptions){.ptFont = gptStarter->get_default_font(), .uColor = PL_COLOR_32_RGBA(1.0f, 1.0f, 1.0f, 1.0f), .fSize = 11.0f});
    }

    const char* pcSpawnMode = ptAppData->bFollowLastSpawn ? "FOLLOW LAST END" : "CENTER";
    char acSpawnLine[128] = {0};
    snprintf(acSpawnLine, sizeof(acSpawnLine), "spawn mode: %s", pcSpawnMode);
    gptDraw->add_text(ptAppData->ptHudLayer, (plVec2){20.0f, 288.0f}, acSpawnLine,
        (plDrawTextOptions){.ptFont = gptStarter->get_default_font(), .uColor = PL_COLOR_32_RGBA(0.95f, 0.95f, 0.95f, 1.0f), .fSize = 12.0f});

    const plVec2 tFollowMin = {20.0f, 304.0f};
    const plVec2 tFollowMax = {128.0f, 328.0f};
    gptDraw->add_rect_filled(
        ptAppData->ptHudLayer,
        tFollowMin,
        tFollowMax,
        (plDrawSolidOptions){.uColor = ptAppData->bFollowLastSpawn
            ? PL_COLOR_32_RGBA(0.20f, 0.45f, 0.20f, 0.90f)
            : PL_COLOR_32_RGBA(0.30f, 0.30f, 0.30f, 0.85f)});
    gptDraw->add_text(ptAppData->ptHudLayer, (plVec2){28.0f, 310.0f}, "FOLLOW LAST",
        (plDrawTextOptions){.ptFont = gptStarter->get_default_font(), .uColor = PL_COLOR_32_RGBA(1.0f, 1.0f, 1.0f, 1.0f), .fSize = 11.0f});

    const plVec2 tCaptureMin = {136.0f, 304.0f};
    const plVec2 tCaptureMax = {252.0f, 328.0f};
    gptDraw->add_rect_filled(
        ptAppData->ptHudLayer,
        tCaptureMin,
        tCaptureMax,
        (plDrawSolidOptions){.uColor = PL_COLOR_32_RGBA(0.30f, 0.30f, 0.30f, 0.85f)});
    gptDraw->add_text(ptAppData->ptHudLayer, (plVec2){144.0f, 310.0f}, "CAPTURE",
        (plDrawTextOptions){.ptFont = gptStarter->get_default_font(), .uColor = PL_COLOR_32_RGBA(1.0f, 1.0f, 1.0f, 1.0f), .fSize = 11.0f});

    const plVec2 tResetMin = {260.0f, 304.0f};
    const plVec2 tResetMax = {350.0f, 328.0f};
    gptDraw->add_rect_filled(
        ptAppData->ptHudLayer,
        tResetMin,
        tResetMax,
        (plDrawSolidOptions){.uColor = PL_COLOR_32_RGBA(0.35f, 0.26f, 0.20f, 0.90f)});
    gptDraw->add_text(ptAppData->ptHudLayer, (plVec2){268.0f, 310.0f}, "RESET NOW",
        (plDrawTextOptions){.ptFont = gptStarter->get_default_font(), .uColor = PL_COLOR_32_RGBA(1.0f, 0.95f, 0.9f, 1.0f), .fSize = 11.0f});
}

static void
pl__draw_bridge_frame(plAppData* ptAppData)
{
    if(!ptAppData->bHasFrame)
        return;

    const BridgeFrame* ptFrame = &ptAppData->tFrame;

    static const uint32_t auSmplEdges[][2] = {
        {0, 1}, {1, 2}, {2, 3}, {3, 4},
        {0, 5}, {5, 6}, {6, 7}, {7, 8},
        {0, 9}, {9, 10}, {10, 11}, {11, 12}, {12, 13},
        {9, 14}, {14, 15}, {15, 16}, {16, 17}, {17, 18},
        {9, 19}, {19, 20}, {20, 21}, {21, 22}, {22, 23}
    };

    if(ptAppData->bShowYellow)
    {
        for(uint32_t i = 0; i < ptFrame->uBodyCount; i++)
        {
            plSphere tSphere = {
                .fRadius = 0.04f,
                .tCenter = ptFrame->atBodyPos[i]
            };
            gptDraw->add_3d_sphere_filled(ptAppData->pt3dDrawlist, tSphere, 0, 0,
                (plDrawSolidOptions){.uColor = PL_COLOR_32_RGBA(0.95f, 0.9f, 0.1f, 1.0f)});
        }
    }

    if(ptAppData->bShowBlue && ptFrame->uBodyCount >= 24)
    {
        for(uint32_t i = 0; i < ARRAY_COUNT(auSmplEdges); i++)
        {
            const uint32_t i0 = auSmplEdges[i][0];
            const uint32_t i1 = auSmplEdges[i][1];
            gptDraw->add_3d_line(ptAppData->pt3dDrawlist, ptFrame->atBodyPos[i0], ptFrame->atBodyPos[i1],
                (plDrawLineOptions){.uColor = PL_COLOR_32_RGBA(0.2f, 0.7f, 1.0f, 1.0f), .fThickness = 4.0f});
        }
    }
    else if(ptAppData->bShowBlue)
    {
        for(uint32_t i = 1; i < ptFrame->uBodyCount; i++)
        {
            gptDraw->add_3d_line(ptAppData->pt3dDrawlist, ptFrame->atBodyPos[i - 1], ptFrame->atBodyPos[i],
                (plDrawLineOptions){.uColor = PL_COLOR_32_RGBA(0.2f, 0.7f, 1.0f, 1.0f), .fThickness = 3.0f});
        }
    }

    plVec3 atPredDraw[MAX_PRED_POINTS] = {0};
    bool bUsePredConverted = false;
    plVec3 tPredOffset = {0.0f, 0.0f, 0.0f};

    if(ptFrame->uPredCount > 0)
    {
        if(ptFrame->uBodyCount > 0)
        {
            const plVec3 tRootBody = ptFrame->atBodyPos[0];
            const plVec3 tRootRaw = ptFrame->atPredPos[0];
            const plVec3 tRootConv = pl__ref_to_sim_vec3(tRootRaw);
            bUsePredConverted = pl__dist_sq_vec3(tRootConv, tRootBody) < pl__dist_sq_vec3(tRootRaw, tRootBody);
            const plVec3 tPredRoot = bUsePredConverted ? tRootConv : tRootRaw;
            tPredOffset = (plVec3){
                tRootBody.x - tPredRoot.x,
                tRootBody.y - tPredRoot.y,
                tRootBody.z - tPredRoot.z,
            };
        }

        for(uint32_t i = 0; i < ptFrame->uPredCount; i++)
        {
            const plVec3 tBase = bUsePredConverted ? pl__ref_to_sim_vec3(ptFrame->atPredPos[i]) : ptFrame->atPredPos[i];
            atPredDraw[i] = (plVec3){
                tBase.x + tPredOffset.x,
                tBase.y + tPredOffset.y,
                tBase.z + tPredOffset.z,
            };
        }
    }

    if(ptAppData->bShowGreen)
    {
        for(uint32_t i = 0; i < ptFrame->uPredCount; i++)
        {
            plSphere tSphere = {
                .fRadius = 0.02f,
                .tCenter = atPredDraw[i]
            };
            gptDraw->add_3d_sphere_filled(ptAppData->pt3dDrawlist, tSphere, 0, 0,
                (plDrawSolidOptions){.uColor = PL_COLOR_32_RGBA(0.2f, 1.0f, 0.4f, 1.0f)});
        }

        if(ptFrame->uPredCount >= 24)
        {
            for(uint32_t i = 0; i < ARRAY_COUNT(auSmplEdges); i++)
            {
                const uint32_t i0 = auSmplEdges[i][0];
                const uint32_t i1 = auSmplEdges[i][1];
                gptDraw->add_3d_line(ptAppData->pt3dDrawlist, atPredDraw[i0], atPredDraw[i1],
                    (plDrawLineOptions){.uColor = PL_COLOR_32_RGBA(0.2f, 1.0f, 0.4f, 1.0f), .fThickness = 2.0f});
            }
        }
        else
        {
            for(uint32_t i = 1; i < ptFrame->uPredCount; i++)
            {
                gptDraw->add_3d_line(ptAppData->pt3dDrawlist, atPredDraw[i - 1], atPredDraw[i],
                    (plDrawLineOptions){.uColor = PL_COLOR_32_RGBA(0.2f, 1.0f, 0.4f, 1.0f), .fThickness = 1.0f});
            }
        }
    }

    for(uint32_t i = 0; i < ptFrame->uDebugSegmentCount; i++)
    {
        gptDraw->add_3d_line(ptAppData->pt3dDrawlist, ptFrame->atDebugSegStart[i], ptFrame->atDebugSegEnd[i],
            (plDrawLineOptions){.uColor = PL_COLOR_32_RGBA(1.0f, 0.2f, 0.2f, 1.0f), .fThickness = 1.0f});
    }
}

static plVec3
pl__get_frame_root(const BridgeFrame* ptFrame)
{
    if(ptFrame && ptFrame->uBodyCount > 0)
        return ptFrame->atBodyPos[0];
    return (plVec3){0.0f, 0.0f, 0.0f};
}

PL_EXPORT void*
pl_app_load(plApiRegistryI* ptApiRegistry, plAppData* ptAppData)
{
    const plDataRegistryI* ptDataRegistry = pl_get_api_latest(ptApiRegistry, plDataRegistryI);
    (void)ptDataRegistry;

    if(ptAppData)
    {
        gptIO      = pl_get_api_latest(ptApiRegistry, plIOI);
        gptWindows = pl_get_api_latest(ptApiRegistry, plWindowI);
        gptGfx     = pl_get_api_latest(ptApiRegistry, plGraphicsI);
        gptDraw    = pl_get_api_latest(ptApiRegistry, plDrawI);
        gptStarter = pl_get_api_latest(ptApiRegistry, plStarterI);

        // Hot-reload safety: do not preserve potentially inverted camera state.
        ptAppData->bAutoFollow = true;
        ptAppData->tOrbitTarget = (plVec3){0.0f, 0.0f, 0.9f};
        ptAppData->fOrbitDist = 8.0f;
        ptAppData->bCameraInitializedFromFrame = false;
        ptAppData->tCamera.fYaw = PL_PI + PL_PI_4;
        ptAppData->tCamera.fPitch = -0.45f;
        ptAppData->tCamera.fRoll = 0.0f;
        ptAppData->bShowYellow = true;
        ptAppData->bShowBlue = false;
        ptAppData->bShowGreen = false;
        ptAppData->bShowRed = true;
        ptAppData->bHudWantsMouse = false;
        ptAppData->bFollowLastSpawn = true;
        pl__camera_orbit_from_angles(&ptAppData->tCamera, ptAppData->tOrbitTarget, ptAppData->fOrbitDist);
        return ptAppData;
    }

    ptAppData = malloc(sizeof(plAppData));
    memset(ptAppData, 0, sizeof(plAppData));
    ptAppData->iSocketFd = -1;
    ptAppData->bAutoFollow = true;
    ptAppData->tOrbitTarget = (plVec3){0.0f, 0.0f, 0.9f};
    ptAppData->fOrbitDist   = 8.0f;
    ptAppData->bCameraInitializedFromFrame = false;
    ptAppData->bShowYellow = true;
    ptAppData->bShowBlue = false;
    ptAppData->bShowGreen = false;
    ptAppData->bShowRed = true;
    ptAppData->bHudWantsMouse = false;
    ptAppData->bFollowLastSpawn = true;

    const plExtensionRegistryI* ptExtensionRegistry = pl_get_api_latest(ptApiRegistry, plExtensionRegistryI);
    ptExtensionRegistry->load("pl_unity_ext", NULL, NULL, true);
    ptExtensionRegistry->load("pl_platform_ext", "pl_load_platform_ext", "pl_unload_platform_ext", false);

    gptIO      = pl_get_api_latest(ptApiRegistry, plIOI);
    gptWindows = pl_get_api_latest(ptApiRegistry, plWindowI);
    gptGfx     = pl_get_api_latest(ptApiRegistry, plGraphicsI);
    gptDraw    = pl_get_api_latest(ptApiRegistry, plDrawI);
    gptStarter = pl_get_api_latest(ptApiRegistry, plStarterI);

    plWindowDesc tWindowDesc = {
        .pcTitle = "CLoSD PilotLight Bridge",
        .iXPos   = 120,
        .iYPos   = 120,
        .uWidth  = 1280,
        .uHeight = 720,
    };
    gptWindows->create(tWindowDesc, &ptAppData->ptWindow);
    gptWindows->show(ptAppData->ptWindow);

    plStarterInit tStarterInit = {
        .tFlags   = PL_STARTER_FLAGS_ALL_EXTENSIONS,
        .ptWindow = ptAppData->ptWindow
    };
    tStarterInit.tFlags |= PL_STARTER_FLAGS_DEPTH_BUFFER;
    tStarterInit.tFlags |= PL_STARTER_FLAGS_MSAA;

    gptStarter->initialize(tStarterInit);
    gptStarter->finalize();

    ptAppData->pt3dDrawlist = gptDraw->request_3d_drawlist();
    ptAppData->ptHudDrawlist = gptDraw->request_2d_drawlist();
    ptAppData->ptHudLayer = gptDraw->request_2d_layer(ptAppData->ptHudDrawlist);

    ptAppData->tCamera = (plCamera){
        .tPos         = {5.0f, 10.0f, 10.0f},
        .fNearZ       = 0.01f,
        .fFarZ        = 200.0f,
        .fFieldOfView = PL_PI_3,
        .fAspectRatio = 1.0f,
        .fYaw         = PL_PI + PL_PI_4,
        .fPitch       = -PL_PI_4,
        .fRoll        = 0.0f,
    };
    pl__camera_orbit_from_angles(&ptAppData->tCamera, ptAppData->tOrbitTarget, ptAppData->fOrbitDist);

    if(!pl__open_bridge_socket(ptAppData))
        printf("[bridge] failed to bind udp://%s:%d\n", BRIDGE_HOST, BRIDGE_PORT);
    else
        printf("[bridge] listening on udp://%s:%d\n", BRIDGE_HOST, BRIDGE_PORT);

    if(!pl__open_control_socket(ptAppData))
        printf("[bridge] failed control socket udp://%s:%d\n", BRIDGE_CONTROL_HOST, BRIDGE_CONTROL_PORT);
    else
        printf("[bridge] control target udp://%s:%d\n", BRIDGE_CONTROL_HOST, BRIDGE_CONTROL_PORT);

    pl__send_spawn_mode(ptAppData);

    return ptAppData;
}

PL_EXPORT void
pl_app_shutdown(plAppData* ptAppData)
{
    pl__close_bridge_socket(ptAppData);
    pl__close_control_socket(ptAppData);
    gptStarter->cleanup();
    gptWindows->destroy(ptAppData->ptWindow);
    free(ptAppData);
}

PL_EXPORT void
pl_app_resize(plWindow* ptWindow, plAppData* ptAppData)
{
    (void)ptWindow;
    gptStarter->resize();

    plIO* ptIO = gptIO->get_io();
    if(ptIO->tMainViewportSize.y > 0.0f)
        ptAppData->tCamera.fAspectRatio = ptIO->tMainViewportSize.x / ptIO->tMainViewportSize.y;
}

PL_EXPORT void
pl_app_update(plAppData* ptAppData)
{
    if(!gptStarter->begin_frame())
        return;

    pl__poll_bridge_socket(ptAppData);

    if(gptIO->is_key_pressed(PL_KEY_F, false))
    {
        ptAppData->bAutoFollow = !ptAppData->bAutoFollow;
        printf("[bridge] auto-follow: %s\n", ptAppData->bAutoFollow ? "on" : "off");
    }

    pl__process_hud_buttons(ptAppData);

    // --- mouse orbit (left-drag rotates, scroll zooms) ---
    static const float fRotSpeed  = 0.005f;
    static const float fPitchMin  = -(PL_PI * 0.49f);
    static const float fPitchMax  =  (PL_PI * 0.49f);

    // left-drag: full orbit (yaw + pitch)
    if(!ptAppData->bHudWantsMouse && gptIO->is_mouse_dragging(PL_MOUSE_BUTTON_LEFT, 1.0f))
    {
        const plVec2 tDelta = gptIO->get_mouse_drag_delta(PL_MOUSE_BUTTON_LEFT, 1.0f);
        ptAppData->tCamera.fYaw   -= tDelta.x * fRotSpeed;
        ptAppData->tCamera.fPitch -= tDelta.y * fRotSpeed;
        ptAppData->tCamera.fYaw    = pl__wrap_angle(ptAppData->tCamera.fYaw);
        ptAppData->tCamera.fPitch  = pl_clampf(fPitchMin, ptAppData->tCamera.fPitch, fPitchMax);
        gptIO->reset_mouse_drag_delta(PL_MOUSE_BUTTON_LEFT);
    }

    // right-drag: standard yaw-only orbit around Z-up axis.
    if(!ptAppData->bHudWantsMouse && gptIO->is_mouse_dragging(PL_MOUSE_BUTTON_RIGHT, 1.0f))
    {
        const plVec2 tDelta = gptIO->get_mouse_drag_delta(PL_MOUSE_BUTTON_RIGHT, 1.0f);

        // Force orbit pivot to the current humanoid root so RMB always rotates around the body.
        if(ptAppData->bHasFrame && ptAppData->tFrame.uBodyCount > 0)
            ptAppData->tOrbitTarget = (plVec3){
                ptAppData->tFrame.atBodyPos[0].x,
                ptAppData->tFrame.atBodyPos[0].y,
                ptAppData->tFrame.atBodyPos[0].z + 0.9f
            };

        ptAppData->tCamera.fYaw = pl__wrap_angle(ptAppData->tCamera.fYaw - tDelta.x * fRotSpeed);

        gptIO->reset_mouse_drag_delta(PL_MOUSE_BUTTON_RIGHT);
    }

    const float fWheel = gptIO->get_mouse_wheel();
    if(fWheel > 0.0f)
        ptAppData->fOrbitDist = pl_clampf(0.5f, ptAppData->fOrbitDist * 0.90f, 100.0f);
    else if(fWheel < 0.0f)
        ptAppData->fOrbitDist = pl_clampf(0.5f, ptAppData->fOrbitDist * 1.10f, 100.0f);

    float fGroundZ = 0.0f;
    plVec3 tGridCenter = {0.0f, 0.0f, 0.0f};
    plVec3 tRootMarker = {0.0f, 0.0f, 0.0f};
    if(ptAppData->bHasFrame)
    {
        fGroundZ = pl__estimate_ground_z(&ptAppData->tFrame);
        tRootMarker = pl__get_frame_root(&ptAppData->tFrame);

        if(ptAppData->bFollowLastSpawn)
        {
            // Keep anchor live with current root while follow mode is active.
            ptAppData->tFrame.tSpawnAnchor = tRootMarker;
            ptAppData->tFrame.bHasSpawnAnchor = true;
        }

        if(ptAppData->tFrame.uBodyCount > 0)
        {
            // Keep the XY grid fixed in world space so root motion is visible against axes.
            tGridCenter = (plVec3){0.0f, 0.0f, fGroundZ};

            // One-time startup framing from live data: upright, above ground, centered on humanoid.
            if(!ptAppData->bCameraInitializedFromFrame)
            {
                ptAppData->tOrbitTarget = ptAppData->tFrame.atBodyPos[0];
                ptAppData->tOrbitTarget.z = fGroundZ + 0.9f;
                ptAppData->fOrbitDist = 8.0f;
                ptAppData->tCamera.fYaw = -PL_PI_4;
                ptAppData->tCamera.fPitch = -0.45f;
                ptAppData->tCamera.fRoll = 0.0f;
                ptAppData->bCameraInitializedFromFrame = true;
            }
        }
    }

    // when auto-follow is on, keep orbit target near the root joint
    if(ptAppData->bAutoFollow && ptAppData->bHasFrame && ptAppData->tFrame.uBodyCount > 0)
    {
        ptAppData->tOrbitTarget   = ptAppData->tFrame.atBodyPos[0];
        ptAppData->tOrbitTarget.z = fGroundZ + 0.9f;

        // Startup guard: if camera is below the character pivot, force an above-target pitch.
        // This avoids upside-down starts while preserving normal controls afterward.
        if(!gptIO->is_mouse_dragging(PL_MOUSE_BUTTON_LEFT, 1.0f) &&
           !gptIO->is_mouse_dragging(PL_MOUSE_BUTTON_RIGHT, 1.0f) &&
           ptAppData->tCamera.tPos.z < (ptAppData->tOrbitTarget.z - 0.1f))
        {
            ptAppData->tCamera.fPitch = -pl_maxf(0.45f, fabsf(ptAppData->tCamera.fPitch));
            ptAppData->tCamera.fPitch = pl_clampf(fPitchMin, ptAppData->tCamera.fPitch, fPitchMax);
        }
    }
    pl__camera_orbit_from_angles(&ptAppData->tCamera, ptAppData->tOrbitTarget, ptAppData->fOrbitDist);

    pl__draw_ground_grid(ptAppData, tGridCenter, fGroundZ);


    // Visible origin marker for camera sanity check.
    if(ptAppData->bShowRed)
    {
        gptDraw->add_3d_sphere_filled(ptAppData->pt3dDrawlist,
            (plSphere){.fRadius = 0.0625f, .tCenter = tRootMarker},
            0,
            0,
            (plDrawSolidOptions){.uColor = PL_COLOR_32_RGBA(1.0f, 0.3f, 0.2f, 1.0f)});
    }

    pl__draw_bridge_frame(ptAppData);

    const double dNow = gptIO->get_io()->dTime;
    if(dNow - ptAppData->dLastStatsPrintTime > 2.0)
    {
        ptAppData->dLastStatsPrintTime = dNow;
        printf("[bridge] stats parsed=%llu failed=%llu has_frame=%d\n",
            (unsigned long long)ptAppData->uPacketsParsed,
            (unsigned long long)ptAppData->uPacketParseFailures,
            ptAppData->bHasFrame ? 1 : 0);
    }

    plIO* ptIO = gptIO->get_io();
    plRenderEncoder* ptEncoder = gptStarter->begin_main_pass();

    pl__draw_hud(ptAppData, fGroundZ);
    gptDraw->submit_2d_layer(ptAppData->ptHudLayer);

    const plMat4 tMVP = pl_mul_mat4(&ptAppData->tCamera.tProjMat, &ptAppData->tCamera.tViewMat);
    gptDraw->submit_3d_drawlist(
        ptAppData->pt3dDrawlist,
        ptEncoder,
        ptIO->tMainViewportSize.x,
        ptIO->tMainViewportSize.y,
        &tMVP,
        PL_DRAW_FLAG_DEPTH_TEST | PL_DRAW_FLAG_DEPTH_WRITE,
        gptGfx->get_swapchain_info(gptStarter->get_swapchain()).tSampleCount
    );

    gptDraw->submit_2d_drawlist(
        ptAppData->ptHudDrawlist,
        ptEncoder,
        ptIO->tMainViewportSize.x,
        ptIO->tMainViewportSize.y,
        gptGfx->get_swapchain_info(gptStarter->get_swapchain()).tSampleCount
    );

    gptStarter->end_main_pass();
    gptStarter->end_frame();
}
