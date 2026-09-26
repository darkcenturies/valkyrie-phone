#include "phone_model.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "log.h"
#include "sprite.h"

namespace phone_model {
namespace {

// RenderWare, v1.0 US.
constexpr uintptr_t kRwStreamOpen = 0x7ECEF0;        // (type, access, data)
constexpr uintptr_t kRwStreamClose = 0x7ECE20;       // (stream, data)
constexpr uintptr_t kRwStreamFindChunk = 0x7ED2D0;   // (stream, type, length*, version*)
constexpr uintptr_t kRpClumpStreamRead = 0x74B420;   // (stream)
constexpr int kStreamFilename = 2, kStreamRead = 1, kChunkClump = 0x10;
// CTxdStore: the dictionary a model's materials find their textures in.
constexpr uintptr_t kPushCurrentTxd = 0x7316A0;
constexpr uintptr_t kPopCurrentTxd = 0x7316B0;
constexpr uintptr_t kSetCurrentTxd = 0x7319C0;  // (slot)
// CModelInfo::ms_modelInfoPtrs, read from CPed::AddWeaponModel's own
// instruction (mov edi, [ebp*4 + array]) rather than taken as a constant: a
// limit adjuster moves the array and patches that instruction to match.
constexpr uintptr_t kModelInfoArrayRef = 0x5E5F0A;
constexpr int kCellphoneModel = 330;
constexpr size_t kRwObject = 0x1C;       // CBaseModelInfo::m_pRwObject
// CClumpModelInfo's vtable (0x85BD30; the weapon and ped ones follow it):
// CreateInstance() is entry 11, SetClump entry 16.
constexpr int kCreateInstanceSlot = 11;
constexpr uintptr_t kClumpCreateInstance = 0x4C5140;
constexpr int kSetClumpSlot = 16;

// RenderWare's material effects (plugin-sdk's RenderWare.cpp; SetEffects is
// also the one SA-MP calls). The game's cars are made shiny this way.
constexpr uintptr_t kRpClumpForAllAtomics = 0x749B70;      // (clump, callback, data)
constexpr uintptr_t kRpGeometryForAllMaterials = 0x74C790;  // (geometry, callback, data)
constexpr uintptr_t kRpMatFXAtomicEnableEffects = 0x811C00;
constexpr uintptr_t kRpMatFXMaterialSetEffects = 0x811C80;  // (material, flags)
constexpr uintptr_t kRpMatFXMaterialSetupEnvMap = 0x811ED0; // (material, texture, frame, fb alpha, coefficient)
constexpr int kMatFXEnvMap = 2;
constexpr size_t kAtomicGeometry = 0x18;  // RpAtomic::geometry
constexpr size_t kObjectParent = 0x4;     // RwObject::parent: a camera's frame
constexpr uintptr_t kSceneCamera = 0xC1703C;

struct Shine {
    uintptr_t texture;
    uintptr_t frame;
    float coefficient;
    int materials;
};

// How much each part of the phone's own model reflects, by its texture
// (tools/phone-model): the polished rim and buttons most, the lens and the
// glass front less, the brushed back softly, the lit screen and the black
// plastic hardly. Any other model's materials all take the ini's Shine.
float PartShine(uintptr_t material) {
    const uintptr_t texture = *reinterpret_cast<const uintptr_t*>(material);  // RpMaterial::texture
    if (!texture) return 1.0f;
    const char* name = reinterpret_cast<const char*>(texture + 0x10);        // RwTexture::name
    static const struct { const char* texture; float share; } kParts[] = {
        {"vp_chrome", 1.0f}, {"vp_lens", 0.8f}, {"vp_front", 0.45f}, {"vp_back", 0.3f},
        {"vp_screen", 0.15f}, {"vp_black", 0.08f},
    };
    for (const auto& p : kParts) {
        if (_strnicmp(name, p.texture, 32) == 0) return p.share;
    }
    return 1.0f;
}

uintptr_t __cdecl ShineMaterial(uintptr_t material, void* data) {
    auto* shine = static_cast<Shine*>(data);
    reinterpret_cast<uintptr_t(__cdecl*)(uintptr_t, int)>(kRpMatFXMaterialSetEffects)(material, kMatFXEnvMap);
    reinterpret_cast<uintptr_t(__cdecl*)(uintptr_t, uintptr_t, uintptr_t, int, float)>(kRpMatFXMaterialSetupEnvMap)(
        material, shine->texture, shine->frame, 0, shine->coefficient * PartShine(material));
    ++shine->materials;
    return material;
}

uintptr_t __cdecl ShineAtomic(uintptr_t atomic, void* data) {
    reinterpret_cast<uintptr_t(__cdecl*)(uintptr_t)>(kRpMatFXAtomicEnableEffects)(atomic);
    if (const uintptr_t geometry = *reinterpret_cast<const uintptr_t*>(atomic + kAtomicGeometry)) {
        reinterpret_cast<uintptr_t(__cdecl*)(uintptr_t, void*, void*)>(kRpGeometryForAllMaterials)(
            geometry, reinterpret_cast<void*>(&ShineMaterial), data);
    }
    return atomic;
}

uintptr_t g_ours = 0;      // our clump, set up as model 330's
uintptr_t g_original = 0;  // the game's, while ours stands in
bool g_using = false;

// Drawing a clump on a ped's bone (as CVisibilityPlugins::RenderWeaponPedsForPC
// does for the right hand).
constexpr uintptr_t kRwEngineInstance = 0xC97B24;         // RwGlobals*; curCamera first
constexpr size_t kRenderStateSet = 0x20, kRenderStateGet = 0x24;  // RwGlobals::dOpenDevice
constexpr int kStateZTest = 6, kStateZWrite = 8, kStateCull = 20;
constexpr uintptr_t kGetAnimHierarchyFromSkinClump = 0x734A40;
constexpr uintptr_t kRpHAnimIDGetIndex = 0x7C51A0;
constexpr int kBoneLeftHand = 34;
constexpr uintptr_t kRwMatrixRotate = 0x7F1FD0;     // (matrix, axis*, degrees, combine)
constexpr uintptr_t kRwMatrixTranslate = 0x7F2450;  // (matrix, v*, combine)
constexpr int kCombinePreconcat = 1;
constexpr uintptr_t kRwFrameUpdateObjects = 0x7F0910;
constexpr uintptr_t kRpClumpRender = 0x749B20;
constexpr uintptr_t kRpClumpClone = 0x749F70;
constexpr uintptr_t kRpClumpDestroy = 0x74A310;
constexpr size_t kEntityClump = 0x18;           // CEntity::m_pRwClump
constexpr int kSetupLightingSlot = 19, kRemoveLightingSlot = 20;  // CEntity's vtable

uintptr_t g_hand = 0;      // the copy drawn in the hand
uintptr_t g_handFrom = 0;  // the clump it was copied from
int g_longAxis = 1;        // the model's longest axis, measured from its vertices
float g_center[3] = {};    // the middle of the model, which is off its origin (the grip)

// Drawn in the game's own weapon pass (CVisibilityPlugins::
// RenderWeaponPedsForPC), right after the world, where the world's depth
// still stands and CJ's body hides what is behind it - as the game draws a
// gun in a ped's hand. What to draw is set each frame by the phone.
constexpr uintptr_t kRenderWeaponPedsForPC = 0x732F30;
constexpr uintptr_t kFindPlayerPed = 0x56E210;
bool g_passHooked = false;
struct HandDraw {
    bool on = false;
    float turn[3] = {0, 0, 0};
    float offset[3] = {0, 0, 0};
    bool flip = true;
} g_draw;

uintptr_t CellphoneInfo() {
    const uintptr_t array = *reinterpret_cast<const uintptr_t*>(kModelInfoArrayRef);
    return array ? reinterpret_cast<const uintptr_t*>(array)[kCellphoneModel] : 0;
}

uintptr_t& RwObject(uintptr_t info) { return *reinterpret_cast<uintptr_t*>(info + kRwObject); }

}  // namespace

bool Load(const std::string& dff, const std::string& txd) {
    if (GetFileAttributesA(dff.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesA(txd.c_str()) == INVALID_FILE_ATTRIBUTES) {
        logfile::Line("phone model: no %s - the game's phone is used", dff.c_str());
        return false;
    }
    const uintptr_t info = CellphoneInfo();
    const int slot = sprite::LoadDictionary("valkyrie_phone_model", txd.c_str());
    if (!info || slot < 0) {
        logfile::Line("phone model: could not load %s", txd.c_str());
        return false;
    }
    // Read the clump with its textures bound from its own dictionary.
    reinterpret_cast<void(__cdecl*)()>(kPushCurrentTxd)();
    reinterpret_cast<void(__cdecl*)(int)>(kSetCurrentTxd)(slot);
    uintptr_t clump = 0;
    void* stream = reinterpret_cast<void*(__cdecl*)(int, int, const void*)>(kRwStreamOpen)(
        kStreamFilename, kStreamRead, dff.c_str());
    if (stream) {
        if (reinterpret_cast<int(__cdecl*)(void*, int, void*, void*)>(kRwStreamFindChunk)(stream, kChunkClump,
                                                                                         nullptr, nullptr)) {
            clump = reinterpret_cast<uintptr_t(__cdecl*)(void*)>(kRpClumpStreamRead)(stream);
        }
        reinterpret_cast<int(__cdecl*)(void*, void*)>(kRwStreamClose)(stream, nullptr);
    }
    reinterpret_cast<void(__cdecl*)()>(kPopCurrentTxd)();
    if (!clump) {
        logfile::Line("phone model: %s is not a model the game can read", dff.c_str());
        return false;
    }
    // Set it up exactly as the game sets up model 330's own - the model
    // info's SetClump - so everything the game does with the phone in CJ's
    // hand treats it as model 330. Then give 330 its own back.
    const uintptr_t before = RwObject(info);
    const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(info);
    // Only a clump model info - checked by the one entry every kind of them
    // shares - is called into; anything else leaves the game's phone alone.
    if (!vtable || reinterpret_cast<const uintptr_t*>(vtable)[kCreateInstanceSlot] != kClumpCreateInstance) {
        logfile::Line("phone model: model 330 is not the cellphone this expects (vtable %08X) - "
                      "the game's phone is used", static_cast<unsigned>(vtable));
        return false;
    }
    const uintptr_t setClump = reinterpret_cast<const uintptr_t*>(vtable)[kSetClumpSlot];
    reinterpret_cast<void(__thiscall*)(uintptr_t, uintptr_t)>(setClump)(info, clump);
    RwObject(info) = before;
    g_ours = clump;
    logfile::Line("phone model: loaded %s", dff.c_str());
    return true;
}

void MakeShiny(uintptr_t envTexture, float coefficient) {
    if (!g_ours || !envTexture) return;
    const uintptr_t camera = *reinterpret_cast<const uintptr_t*>(kSceneCamera);
    // The reflection turns with the game's camera, as a car's does.
    Shine shine{envTexture, camera ? *reinterpret_cast<const uintptr_t*>(camera + kObjectParent) : 0, coefficient, 0};
    if (!shine.frame) {
        logfile::Line("phone model: no camera to reflect from yet");
        return;
    }
    reinterpret_cast<uintptr_t(__cdecl*)(uintptr_t, void*, void*)>(kRpClumpForAllAtomics)(
        g_ours, reinterpret_cast<void*>(&ShineAtomic), &shine);
    logfile::Line("phone model: %d materials reflect (%.2f)", shine.materials, coefficient);
}

bool Loaded() { return g_ours != 0; }

void Use(bool ours) {
    if (!g_ours || ours == g_using) return;
    const uintptr_t info = CellphoneInfo();
    if (!info) return;
    if (ours) {
        g_original = RwObject(info);
        RwObject(info) = g_ours;
    } else {
        // Only if nothing has replaced ours meanwhile - the streaming reloading
        // the model, say.
        if (RwObject(info) == g_ours) RwObject(info) = g_original;
        g_original = 0;
    }
    g_using = ours;
}

namespace {

struct Matrix {
    float right[3], flags;
    float up[3], pad1;
    float at[3], pad2;
    float pos[3], pad3;
};

struct Box {
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    int verts = 0;
};

// Every vertex of an atomic's geometry into the box (RpGeometry: numVertices
// at +0x14, morphTarget at +0x5C; RpMorphTarget: verts at +0x14).
uintptr_t __cdecl MeasureAtomic(uintptr_t atomic, void* data) {
    auto* box = static_cast<Box*>(data);
    const uintptr_t geometry = *reinterpret_cast<const uintptr_t*>(atomic + kAtomicGeometry);
    if (!geometry) return atomic;
    const int count = *reinterpret_cast<const int*>(geometry + 0x14);
    const uintptr_t morph = *reinterpret_cast<const uintptr_t*>(geometry + 0x5C);
    const float* verts = morph ? *reinterpret_cast<const float* const*>(morph + 0x14) : nullptr;
    if (!verts || count <= 0 || count > 200000) return atomic;
    for (int i = 0; i < count; ++i) {
        for (int k = 0; k < 3; ++k) {
            box->lo[k] = std::min(box->lo[k], verts[i * 3 + k]);
            box->hi[k] = std::max(box->hi[k], verts[i * 3 + k]);
        }
    }
    box->verts += count;
    return atomic;
}

// The left hand's bone as the ped was last animated, and the phone's place
// in it. `flip` turns the phone half round about its own long axis, first,
// so its screen faces the other way and it stays the same way up.
bool HandMatrix(uintptr_t ped, const float turn[3], const float offset[3], bool flip, Matrix& out) {
    const uintptr_t clump = ped ? *reinterpret_cast<const uintptr_t*>(ped + kEntityClump) : 0;
    if (!clump) return false;
    const uintptr_t hierarchy = reinterpret_cast<uintptr_t(__cdecl*)(uintptr_t)>(kGetAnimHierarchyFromSkinClump)(clump);
    if (!hierarchy) return false;
    const int index = reinterpret_cast<int(__cdecl*)(uintptr_t, int)>(kRpHAnimIDGetIndex)(hierarchy, kBoneLeftHand);
    const int nodes = *reinterpret_cast<const int*>(hierarchy + 4);
    const uintptr_t matrices = *reinterpret_cast<const uintptr_t*>(hierarchy + 8);  // pMatrixArray
    if (index < 0 || index >= nodes || !matrices) return false;
    memcpy(&out, reinterpret_cast<const void*>(matrices + index * sizeof(Matrix)), sizeof out);
    static const float axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    auto rotate = reinterpret_cast<void*(__cdecl*)(void*, const float*, float, int)>(kRwMatrixRotate);
    auto translate = reinterpret_cast<void*(__cdecl*)(void*, const float*, int)>(kRwMatrixTranslate);
    translate(&out, offset, kCombinePreconcat);
    for (int i = 0; i < 3; ++i) {
        if (turn[i] != 0.0f) rotate(&out, axes[i], turn[i], kCombinePreconcat);
    }
    if (flip) {
        // About the phone's own middle: the model's origin is the grip, off
        // to one side, and turning about that swings it out of the hand.
        const float back[3] = {-g_center[0], -g_center[1], -g_center[2]};
        translate(&out, g_center, kCombinePreconcat);
        rotate(&out, axes[g_longAxis], 180.0f, kCombinePreconcat);
        translate(&out, back, kCombinePreconcat);
    }
    return true;
}

bool DrawHand(uintptr_t ped, const float turn[3], const float offset[3], bool flip) {
    const uintptr_t engine = *reinterpret_cast<const uintptr_t*>(kRwEngineInstance);
    // Only inside a camera's frame.
    if (!engine || !*reinterpret_cast<const uintptr_t*>(engine) || !ped) return false;
    const uintptr_t info = CellphoneInfo();
    const uintptr_t source = info ? RwObject(info) : 0;
    if (!source) return false;
    if (g_hand && g_handFrom != source) ReleaseHand();
    if (!g_hand) {
        g_hand = reinterpret_cast<uintptr_t(__cdecl*)(uintptr_t)>(kRpClumpClone)(source);
        g_handFrom = source;
        if (!g_hand) return false;
        // Which way is its length: the flip turns it about that.
        Box box;
        reinterpret_cast<uintptr_t(__cdecl*)(uintptr_t, void*, void*)>(kRpClumpForAllAtomics)(
            g_hand, reinterpret_cast<void*>(&MeasureAtomic), &box);
        if (box.verts > 0) {
            const float size[3] = {box.hi[0] - box.lo[0], box.hi[1] - box.lo[1], box.hi[2] - box.lo[2]};
            g_longAxis = size[0] >= size[1] && size[0] >= size[2] ? 0 : size[1] >= size[2] ? 1 : 2;
            for (int k = 0; k < 3; ++k) g_center[k] = (box.lo[k] + box.hi[k]) * 0.5f;
            logfile::Line("phone model: %.3f x %.3f x %.3f, longest along %c", size[0], size[1], size[2],
                          "xyz"[g_longAxis]);
        } else {
            logfile::Line("phone model: its vertices could not be read - turned about y");
        }
    }
    Matrix m{};
    if (!HandMatrix(ped, turn, offset, flip, m)) return false;
    const uintptr_t frame = *reinterpret_cast<const uintptr_t*>(g_hand + kObjectParent);
    if (!frame) return false;
    memcpy(reinterpret_cast<void*>(frame + 0x10), &m, sizeof m);  // RwFrame's modelling matrix
    reinterpret_cast<void*(__cdecl*)(uintptr_t)>(kRwFrameUpdateObjects)(frame);

    using SetFn = int(__cdecl*)(int, void*);
    using GetFn = int(__cdecl*)(int, void*);
    auto set = *reinterpret_cast<SetFn*>(engine + kRenderStateSet);
    auto get = *reinterpret_cast<GetFn*>(engine + kRenderStateGet);
    uintptr_t zTest = 0, zWrite = 0, cull = 0;
    get(kStateZTest, &zTest);
    get(kStateZWrite, &zWrite);
    get(kStateCull, &cull);
    set(kStateZTest, reinterpret_cast<void*>(1));
    set(kStateZWrite, reinterpret_cast<void*>(1));
    set(kStateCull, reinterpret_cast<void*>(1));  // rwCULLMODECULLNONE
    const uintptr_t* vtable = *reinterpret_cast<const uintptr_t* const*>(ped);
    const bool lit = reinterpret_cast<bool(__thiscall*)(uintptr_t)>(vtable[kSetupLightingSlot])(ped);
    reinterpret_cast<void*(__cdecl*)(uintptr_t)>(kRpClumpRender)(g_hand);
    reinterpret_cast<void(__thiscall*)(uintptr_t, bool)>(vtable[kRemoveLightingSlot])(ped, lit);
    set(kStateZTest, reinterpret_cast<void*>(zTest));
    set(kStateZWrite, reinterpret_cast<void*>(zWrite));
    set(kStateCull, reinterpret_cast<void*>(cull));
    return true;
}

// The game's weapon pass, and the phone after it.
void __cdecl WeaponPass() {
    reinterpret_cast<void(__cdecl*)()>(kRenderWeaponPedsForPC)();
    // Not in the mirror render (CMirrors::bRenderingReflection): that is the
    // Camera app's lens, which is in the phone and does not see it.
    if (g_draw.on && !*reinterpret_cast<const bool*>(0xC7C728)) {
        const uintptr_t ped = reinterpret_cast<uintptr_t(__cdecl*)(int)>(kFindPlayerPed)(-1);
        DrawHand(ped, g_draw.turn, g_draw.offset, g_draw.flip);
    }
}

}  // namespace

bool HookWeaponPass() {
    if (g_passHooked) return true;
    // The frame's own calls to it (RenderScene to Idle); the mirror's and the
    // photo's are elsewhere and left alone.
    int found = 0;
    for (uintptr_t at = 0x53DF40; at < 0x53F000; ++at) {
        if (*reinterpret_cast<const uint8_t*>(at) != 0xE8) continue;
        const uintptr_t target = at + 5 + *reinterpret_cast<const int32_t*>(at + 1);
        if (target != kRenderWeaponPedsForPC) continue;
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&WeaponPass) - (at + 5));
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(at + 1), 4, PAGE_EXECUTE_READWRITE, &old)) continue;
        *reinterpret_cast<int32_t*>(at + 1) = rel;
        VirtualProtect(reinterpret_cast<void*>(at + 1), 4, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), 5);
        ++found;
        at += 4;
    }
    g_passHooked = found > 0;
    logfile::Line("phone model: %s", g_passHooked ? "drawn in the game's weapon pass"
                                                   : "the weapon pass was not found - drawn over the frame instead");
    return g_passHooked;
}

void SetHand(bool on, const float turn[3], const float offset[3], bool flip) {
    g_draw.on = on;
    for (int i = 0; i < 3; ++i) {
        g_draw.turn[i] = turn[i];
        g_draw.offset[i] = offset[i];
    }
    g_draw.flip = flip;
}

bool RenderInLeftHand(uintptr_t ped, const float turn[3], const float offset[3], bool flip) {
    // Drawn in the weapon pass already.
    if (g_passHooked) return true;
    return DrawHand(ped, turn, offset, flip);
}

bool LeftHandPlace(uintptr_t ped, const float turn[3], const float offset[3], bool flip, float pos[3], float facing[3]) {
    Matrix m{};
    if (!HandMatrix(ped, turn, offset, flip, m)) return false;
    for (int i = 0; i < 3; ++i) {
        pos[i] = m.pos[i];
        facing[i] = m.at[i];
    }
    return true;
}

void ReleaseHand() {
    g_draw.on = false;
    if (g_hand) reinterpret_cast<int(__cdecl*)(uintptr_t)>(kRpClumpDestroy)(g_hand);
    g_hand = 0;
    g_handFrom = 0;
}

}  // namespace phone_model
