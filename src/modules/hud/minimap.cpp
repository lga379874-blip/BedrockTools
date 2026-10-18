#include "minimap.hpp"
#include "modules/ModuleRegistry.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>
#include <bedrocktools/sdk/render/Block.hpp>
#include <string_view>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace sdk = bedrocktools::sdk;

// ---- game function pointers (same signatures other BedrockTools modules use) ----
struct BlockPosRaw { int x, y, z; };
using GetRuntimeActorList_t = std::vector<void*>(*)(void*);
using ActorIsPlayer_t       = bool(*)(void*);
using ActorGetNameTag_t     = std::string(*)(void*);
using GetBlock_t            = void*(*)(void* region, const BlockPosRaw& pos);

static GetRuntimeActorList_t s_getActorList = nullptr;
static ActorIsPlayer_t       s_isPlayer     = nullptr;
static ActorGetNameTag_t     s_getNameTag   = nullptr;
static GetBlock_t            s_getBlock     = nullptr;

MiniMapModule* MiniMapModule::s_instance = nullptr;

MiniMapModule::MiniMapModule() : Module("MiniMap", "Mini map with terrain, mob and player heads, waypoints.") {
    s_instance = this;
}
MiniMapModule::~MiniMapModule() { if (s_instance == this) s_instance = nullptr; }
MiniMapModule* MiniMapModule::getInstance() { return s_instance; }

// ---- helpers --------------------------------------------------------------------
static std::string cleanName(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size();) {
        auto c = (unsigned char)in[i];
        if (c == 0xC2 && i + 1 < in.size() && (unsigned char)in[i + 1] == 0xA7) { i += 3; continue; }
        if (c >= 0x20 && c != 0x7F) out.push_back(in[i]);
        ++i;
    }
    auto a = out.find_first_not_of(' ');
    if (a == std::string::npos) return {};
    out = out.substr(a, out.find_last_not_of(' ') - a + 1);
    if (out.size() > 32) out.resize(32);
    return out;
}

static std::string playerName(void* actor) {
    if (s_getNameTag) { auto n = cleanName(s_getNameTag(actor)); if (!n.empty()) return n; }
    auto* p = reinterpret_cast<const std::string*>((uintptr_t)actor + sdk::offsets::Player::mName);
    return p && p->size() <= 256 ? cleanName(*p) : std::string{};
}

// Reads the 8x8 face (+hat layer) from the player's skin, scaled to out (size x size RGBA).
static bool skinHead(void* actor, uint8_t* out, int size) {
    auto skinRef = *reinterpret_cast<void**>((uintptr_t)actor + sdk::offsets::Player::mSkin);
    if (!skinRef) return false;
    auto owner = *reinterpret_cast<void**>((uintptr_t)skinRef + sdk::offsets::SerializedSkinRef::mSkinImpl);
    if (!owner) return false;
    auto impl = (uintptr_t)owner + sdk::offsets::ThreadOwner::mObject;
    auto image = (uintptr_t)impl + sdk::offsets::SerializedSkinImpl::mSkinImage;

    if (*reinterpret_cast<const bool*>(impl + sdk::offsets::SerializedSkinImpl::mIsPersona)) {
        auto b = *reinterpret_cast<const uintptr_t*>(impl + sdk::offsets::SerializedSkinImpl::mSkinAnimatedImages);
        auto e = *reinterpret_cast<const uintptr_t*>(impl + sdk::offsets::SerializedSkinImpl::mSkinAnimatedImages + sizeof(uintptr_t));
        if (b && e >= b && e - b <= sdk::offsets::AnimatedImageData::Size * 64)
            for (auto it = b; it < e; it += sdk::offsets::AnimatedImageData::Size) {
                auto t = *reinterpret_cast<const uint32_t*>(it + sdk::offsets::AnimatedImageData::mType);
                if (t == 2 || t == 3) { image = it + sdk::offsets::AnimatedImageData::mImage; if (t == 3) break; }
            }
    }
    auto w = *reinterpret_cast<const uint32_t*>(image + sdk::offsets::SkinImage::mWidth);
    auto h = *reinterpret_cast<const uint32_t*>(image + sdk::offsets::SkinImage::mHeight);
    auto px = *reinterpret_cast<const uint8_t* const*>(image + sdk::offsets::Image::mBytesOffset);
    if (!px || w < 64 || w > 256 || h < 32 || h > 256 || w % 64 || h % 32) return false;

    const int sc = (int)(w / 64), up = size / 8;
    std::memset(out, 0, (size_t)size * size * 4);
    for (int layer = 0; layer < 2; ++layer) {
        int sx0 = (layer == 0 ? 8 : 40) * sc;
        for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
            const uint8_t* s = px + ((size_t)(8 * sc + y * sc + sc / 2) * w + sx0 + x * sc + sc / 2) * 4;
            if (layer == 1 && s[3] < 128) continue;
            for (int yy = 0; yy < up; ++yy) for (int xx = 0; xx < up; ++xx) {
                uint8_t* d = out + (((size_t)(y * up + yy)) * size + x * up + xx) * 4;
                std::memcpy(d, s, 3); d[3] = 255;
            }
        }
    }
    return true;
}

// ---- mob identification -----------------------------------------------------------
// BedrockTools has no type-name accessor for actors, so mobs are recognised by their
// vanilla hitbox size. Mobs that share a size (zombie/villager/witch) share an icon.
struct MobDef { float w, h; const char* name; uint32_t base, accent; int style; };
static const MobDef kMobs[] = {
    {0.60f, 1.95f, "Zombie",      0x5B8C3A, 0x2E4A9C, 0}, {0.60f, 1.99f, "Skeleton",    0xCFCFCF, 0x303030, 0},
    {0.60f, 1.70f, "Creeper",     0x4FB04A, 0x101010, 1}, {1.40f, 0.90f, "Spider",      0x3B2E28, 0xE02020, 3},
    {0.70f, 0.50f, "Cave Spider", 0x1F4B5A, 0xE02020, 3}, {0.60f, 2.90f, "Enderman",    0x151515, 0xD050FF, 4},
    {0.90f, 0.90f, "Pig",         0xF0A0A8, 0xD07080, 2}, {0.90f, 1.40f, "Cow",         0x4A3A2C, 0xE8E8E8, 6},
    {0.90f, 1.30f, "Sheep",       0xE8E8E8, 0x8A7060, 6}, {0.40f, 0.70f, "Chicken",     0xF4F4F4, 0xF0A020, 5},
    {1.40f, 1.60f, "Horse",       0x8A5A3A, 0x2A1A10, 2}, {0.60f, 0.85f, "Wolf",        0xC8C8C8, 0x202020, 2},
    {0.48f, 0.56f, "Cat",         0xD8A050, 0x202020, 2}, {0.50f, 0.90f, "Bat",         0x4A3A30, 0xE0E0E0, 7},
    {1.40f, 2.70f, "Iron Golem",  0xB8B8B0, 0x5A8A3A, 0}, {0.60f, 1.80f, "Blaze",       0xF0B020, 0x802000, 0},
    {4.00f, 4.00f, "Ghast",       0xF0F0F0, 0x909090, 7}, {0.52f, 0.52f, "Slime S",     0x60C060, 0x207020, 7},
    {1.04f, 1.04f, "Slime M",     0x60C060, 0x207020, 7}, {2.04f, 2.04f, "Slime L",     0x60C060, 0x207020, 7},
    {0.90f, 0.50f, "Phantom",     0x3A4A8A, 0x60F080, 7}, {0.80f, 0.80f, "Squid",       0x304060, 0xE0E0E0, 7},
    {0.70f, 0.60f, "Bee",         0xF0C030, 0x201810, 7}, {0.40f, 0.50f, "Rabbit",      0xB08A60, 0x202020, 2},
    {0.60f, 0.70f, "Fox",         0xE07020, 0xF4F4F4, 2}, {0.70f, 2.40f, "Wither Skel.", 0x303030, 0xB0B0B0, 0},
    {0.60f, 1.80f, "Player-like", 0x806040, 0x202020, 0},
};
static const MobDef kUnknown = {0, 0, "Mob", 0x909090, 0x303030, 0};

static int matchMob(float w, float h) {
    int best = -1; float bd = 0.09f;
    for (int i = 0; i < (int)(sizeof(kMobs) / sizeof(kMobs[0])); ++i) {
        float d = std::max(std::fabs(kMobs[i].w - w), std::fabs(kMobs[i].h - h));
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

// Draws an 8x8 face for a mob (scaled to size x size RGBA).
static void mobFace(const MobDef& m, uint8_t* out, int size) {
    auto rgb = [](uint32_t c, float k, uint8_t* o) { o[0] = (uint8_t)(((c >> 16) & 255) * k); o[1] = (uint8_t)(((c >> 8) & 255) * k); o[2] = (uint8_t)((c & 255) * k); };
    const int up = size / 8;
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
        uint8_t c[3]; rgb(m.base, (x == 0 || y == 0 || x == 7 || y == 7) ? 0.7f : 1.f, c);
        auto set = [&](uint32_t col) { rgb(col, 1.f, c); };
        switch (m.style) {
            case 0: if (y == 3 && (x == 2 || x == 5)) set(0xFFFFFF); if (y == 4 && (x == 2 || x == 5)) set(m.accent); if (y == 6 && x >= 2 && x <= 5) set(0x202020); break;
            case 1: if ((y == 2 || y == 3) && (x == 1 || x == 2 || x == 5 || x == 6)) set(m.accent);
                    if (y == 4 && (x == 3 || x == 4)) set(m.accent); if (y >= 5 && (x == 2 || x == 5 || (y == 5 && x == 3) || (y == 5 && x == 4) )) set(m.accent); break;
            case 2: if (y == 3 && (x == 2 || x == 5)) set(0x202020); if (y >= 5 && y <= 6 && x >= 2 && x <= 5) set(m.accent);
                    if (y == 6 && (x == 3 || x == 4)) set(0x202020); break;
            case 3: if (y >= 2 && y <= 3 && (x == 1 || x == 2 || x == 5 || x == 6)) set(m.accent); break;
            case 4: if (y == 4 && x != 0 && x != 7) set(m.accent); break;
            case 5: if (y == 3 && (x == 1 || x == 6)) set(0x101010); if (y >= 4 && y <= 5 && x >= 3 && x <= 4) set(m.accent); break;
            case 6: if (y == 0 && (x == 0 || x == 7)) set(m.accent); if (y == 3 && (x == 2 || x == 5)) set(0x101010);
                    if (y >= 5 && x >= 2 && x <= 5) set(m.accent); break;
            default: if (y == 3 && (x == 2 || x == 5)) set(m.accent); if (y == 5 && x >= 2 && x <= 5) set(m.accent); break;
        }
        for (int yy = 0; yy < up; ++yy) for (int xx = 0; xx < up; ++xx) {
            uint8_t* d = out + (((size_t)(y * up + yy)) * size + x * up + xx) * 4;
            d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = 255;
        }
    }
}

static uint32_t blockColor(const void* block, float shade, uint8_t* rgb) {
    uint32_t h = (uint32_t)(((uintptr_t)block >> 4) * 2654435761u);
    rgb[0] = (uint8_t)((60 + (h & 0x8F)) * shade);
    rgb[1] = (uint8_t)((60 + ((h >> 8) & 0x8F)) * shade);
    rgb[2] = (uint8_t)((60 + ((h >> 16) & 0x8F)) * shade);
    return h;
}

// ---- natural block colours --------------------------------------------------------
static constexpr uint32_t kSkip = 0x80000000u;        // see-through / decoration: keep scanning down
static constexpr uint32_t kWater = 0x3F76E4;

static uint32_t classifyBlock(const void* block) {
    const std::string* fn = static_cast<const sdk::Block*>(block)->fullName();
    if (!fn || fn->empty() || fn->size() > 256) return 0x808080;
    std::string_view n(*fn);
    if (n.starts_with("minecraft:")) n.remove_prefix(10);
    auto has = [&](std::string_view s) { return n.find(s) != std::string_view::npos; };

    // decoration and air: not part of the terrain surface
    static const char* skip[] = {"air", "tallgrass", "short_grass", "tall_grass", "double_plant", "fern", "sapling",
        "dead_bush", "seagrass", "kelp", "flower", "tulip", "orchid", "dandelion", "poppy", "allium", "bluet",
        "cornflower", "lily", "oxeye", "rose", "torch", "vine", "rail", "light_block", "structure_void", "fire",
        "button", "sign", "pressure_plate", "lever", "web", "carpet", "moss_carpet", "pink_petals", "wither_rose"};
    for (auto* s : skip) if (has(s)) return kSkip;

    if (has("water")) return kWater;
    if (has("lava")) return 0xE8641A;
    if (has("magma")) return 0x8F3A14;
    if (has("blue_ice")) return 0x74A8FF;
    if (has("packed_ice")) return 0x8DB4F5;
    if (has("ice")) return 0x91B8FF;

    if (has("ore")) {
        if (has("coal")) return 0x2E2E2E;   if (has("iron")) return 0xD8AF93;  if (has("gold")) return 0xFCEE4B;
        if (has("diamond")) return 0x5DECF5; if (has("redstone")) return 0xFF2020; if (has("lapis")) return 0x1B4DB5;
        if (has("emerald")) return 0x17DD62; if (has("copper")) return 0xE0775A; if (has("quartz")) return 0xE8E0D4;
    }
    if (has("ancient_debris")) return 0x5A3A30;

    if (has("cherry_leaves")) return 0xF0A8C8;
    if (has("leaves")) return 0x3B7A2E;
    if (n == "grass_block" || n == "grass") return 0x5D9B3A;
    if (has("path")) return 0x947B49;
    if (has("podzol")) return 0x6B4A2A;
    if (has("mycelium")) return 0x6F6377;
    if (has("farmland")) return 0x6B4A2B;
    if (has("mud")) return 0x4A3B35;
    if (has("dirt")) return 0x8B5A33;
    if (has("clay") && !has("hardened")) return 0x9AA3B5;
    if (has("gravel")) return 0x8A8583;
    if (has("soul_")) return 0x5A4636;
    if (has("red_sandstone")) return 0xB5602A;
    if (has("sandstone")) return 0xD8CB9A;
    if (has("red_sand")) return 0xBE6B30;
    if (has("sand")) return 0xE0D49A;
    if (has("snow")) return 0xF4F8FF;

    if (has("log") || has("wood") || has("stem")) {
        if (has("spruce")) return 0x3E2912;  if (has("birch")) return 0xD7D3B0;  if (has("dark_oak")) return 0x3A2A15;
        if (has("jungle")) return 0x5A4420;  if (has("acacia")) return 0x8A5A3A; if (has("cherry")) return 0xE0A0B0;
        if (has("mangrove")) return 0x6A3A30; if (has("crimson")) return 0x7A2A4A; if (has("warped")) return 0x2A7A7A;
        return 0x6B5230;
    }
    if (has("planks") || has("bookshelf")) return 0xA8844F;

    static const struct { const char* name; uint32_t rgb; } dyes[] = {
        {"light_blue", 0x3AB3DA}, {"light_gray", 0x9D9D97}, {"silver", 0x9D9D97}, {"white", 0xE4E4E4}, {"orange", 0xF9801D},
        {"magenta", 0xC74EBD}, {"yellow", 0xFED83D}, {"lime", 0x80C71F}, {"pink", 0xF38BAA}, {"gray", 0x474F52},
        {"cyan", 0x169C9C}, {"purple", 0x8932B8}, {"blue", 0x3C44AA}, {"brown", 0x835432}, {"green", 0x5E7C16},
        {"red", 0xB02E26}, {"black", 0x1D1D21}};
    for (auto& d : dyes) if (n.starts_with(d.name) && n.size() > std::strlen(d.name) && n[std::strlen(d.name)] == '_') return d.rgb;
    if (has("terracotta") || has("hardened_clay")) return 0x9A5B3F;

    if (has("deepslate")) return 0x4D4D52;
    if (has("blackstone")) return 0x2A2530;
    if (has("basalt")) return 0x4A4A50;
    if (has("netherrack")) return 0x6F3030;
    if (has("nether_brick")) return 0x2C1518;
    if (has("end_stone")) return 0xDDDDA0;
    if (has("obsidian")) return 0x1A1226;
    if (has("bedrock")) return 0x333333;
    if (has("tuff")) return 0x6A6A60;
    if (has("calcite")) return 0xE0E0DA;
    if (has("dripstone")) return 0x7F6A58;
    if (has("granite")) return 0x9A6B5A;
    if (has("diorite")) return 0xBDBDBD;
    if (has("andesite")) return 0x888888;
    if (has("cactus")) return 0x3E8A2B;
    if (has("pumpkin")) return 0xC77A12;
    if (has("melon")) return 0x7BA832;
    if (has("hay")) return 0xC8A020;
    if (has("glass")) return 0xA8D8E8;
    if (has("brick")) return 0x9A5544;
    if (has("iron_block")) return 0xD8D8D8;
    if (has("gold_block")) return 0xF8D838;
    if (has("diamond_block")) return 0x62EDE4;
    if (has("stone") || has("cobble")) return 0x7D7D7D;

    uint8_t t[3]; blockColor(block, 1.f, t);          // unknown block: stable fallback colour
    return ((uint32_t)t[0] << 16) | ((uint32_t)t[1] << 8) | t[2];
}

// ---- lifecycle ------------------------------------------------------------------
void MiniMapModule::onInit() {
    using bedrocktools::memory::resolve;
    using bedrocktools::memory::SignatureId;
    s_getActorList = (GetRuntimeActorList_t)resolve(SignatureId::ActorManagerList);
    s_isPlayer     = (ActorIsPlayer_t)resolve(SignatureId::ActorIsPlayer);
    s_getNameTag   = (ActorGetNameTag_t)resolve(SignatureId::ActorGetNameTag);
    s_getBlock     = (GetBlock_t)resolve(SignatureId::BlockSourceGetBlock);
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& e) {
        if (auto* m = MiniMapModule::getInstance()) if (m->enabled) m->onLocalPlayerTick(e.player);
    });
}
void MiniMapModule::onEnable() { m_tick = 0; }
void MiniMapModule::onDisable() { std::lock_guard<std::mutex> l(m_mutex); m_blips.clear(); m_haveSelf = false; }

// ---- data collection (game thread) ----------------------------------------------
void MiniMapModule::onLocalPlayerTick(sdk::Player* lp) {
    if (!lp) return;
    ++m_tick;
    auto pos = lp->position();
    auto rot = lp->rotation();
    { std::lock_guard<std::mutex> l(m_mutex); m_selfX = pos.x; m_selfY = pos.y; m_selfZ = pos.z; m_selfYaw = rot.y; m_haveSelf = true; }

    sampleTerrain(lp);
    if (m_tick % 4) return;

    std::vector<void*> actors;
    if (auto* level = lp->level(); level && s_getActorList)
        if (auto* mgr = level->actorManager()) actors = s_getActorList(mgr);

    std::vector<Blip> next;
    for (void* a : actors) {
        if (!a || a == (void*)lp) continue;
        auto* actor = reinterpret_cast<sdk::Actor*>(a);
        auto p = actor->position();
        auto bb = actor->bounds();
        float w = bb.max.x - bb.min.x, h = bb.max.y - bb.min.y;
        if (h <= 0.05f) continue;

        Blip b; b.x = p.x; b.y = p.y; b.z = p.z;
        b.isPlayer = s_isPlayer && s_isPlayer(a);
        if (b.isPlayer) {
            b.name = playerName(a);
            if (b.name.empty()) continue;
            b.sneaking = h > 1.3f && h < 1.65f;   // standing 1.8, sneaking 1.5
            char key[48]; std::snprintf(key, sizeof key, "mm_p_%llx", (unsigned long long)(uintptr_t)a);
            b.imageId = key;
            int& last = m_headTick[b.imageId];
            if (last == 0 || m_tick - last > 200) {
                static uint8_t buf[HEAD_PX * HEAD_PX * 4];
                if (skinHead(a, buf, HEAD_PX)) {
                    std::array<uint8_t, HEAD_PX * HEAD_PX * 4> arr; std::memcpy(arr.data(), buf, arr.size());
                    pl::modmenu::registerImage(b.imageId, arr, HEAD_PX, HEAD_PX);
                }
                last = m_tick;
            }
        } else {
            if (h < 0.6f || w > 5.f) continue;               // skip items, arrows, xp orbs, boats
            int idx = matchMob(w, h);
            const MobDef& def = idx >= 0 ? kMobs[idx] : kUnknown;
            b.name = def.name;
            b.imageId = "mm_m_" + std::to_string(idx);
            if (m_mobIcons.insert(b.imageId).second) {
                std::array<uint8_t, HEAD_PX * HEAD_PX * 4> arr; mobFace(def, arr.data(), HEAD_PX);
                pl::modmenu::registerImage(b.imageId, arr, HEAD_PX, HEAD_PX);
            }
        }
        next.push_back(std::move(b));
    }
    std::lock_guard<std::mutex> l(m_mutex);
    m_blips.swap(next);
}

void MiniMapModule::sampleTerrain(sdk::Player* lp) {
    if (!s_getBlock) return;
    auto* dim = lp->dimension();
    void* region = dim ? dim->blockSource() : nullptr;
    if (!region) return;

    auto pos = lp->position();
    const float zoom = m_zoom, bpc = zoom / GRID;
    const int cx = (int)std::floor(pos.x), cz = (int)std::floor(pos.z);
    const int startY = m_underground ? (int)m_yLevel : std::min(319, (int)pos.y + 24);

    if (!m_airBlock) m_airBlock = s_getBlock(region, BlockPosRaw{cx, 319, cz});

    const int perTick = 384;
    for (int n = 0; n < perTick; ++n) {
        int idx = (m_sampleCursor + n) % (GRID * GRID);
        int gx = idx % GRID, gz = idx / GRID;
        int wx = (int)std::floor(pos.x + (gx - GRID / 2) * bpc);
        int wz = (int)std::floor(pos.z + (gz - GRID / 2) * bpc);

        uint8_t rgb[3] = {20, 20, 20};
        int hitY = startY - 47;
        for (int d = 0; d < 48; ++d) {
            const int y = startY - d;
            void* b = s_getBlock(region, BlockPosRaw{wx, y, wz});
            if (!b || b == m_airBlock) continue;
            uint32_t col;
            auto it = m_blockColors.find(b);
            if (it == m_blockColors.end()) { col = classifyBlock(b); m_blockColors.emplace(b, col); } else col = it->second;
            if (col & kSkip) continue;

            float shade = 1.0f - std::min(d, 32) / 80.f;
            if (col == kWater) {                                   // deeper water = darker blue
                int t = 1; while (t < 16 && s_getBlock(region, BlockPosRaw{wx, y - t, wz}) == b) ++t;
                shade = 1.0f - t * 0.045f;
            } else if (gx > 0 && gz > 0) {                         // relief: lighter on slopes facing up-left
                float rel = ((y - m_heights[idx - 1]) + (y - m_heights[idx - GRID])) * 0.03f;
                shade *= 1.0f + std::clamp(rel, -0.25f, 0.25f);
            }
            hitY = y;
            rgb[0] = (uint8_t)std::clamp(((col >> 16) & 255) * shade, 0.f, 255.f);
            rgb[1] = (uint8_t)std::clamp(((col >> 8) & 255) * shade, 0.f, 255.f);
            rgb[2] = (uint8_t)std::clamp((col & 255) * shade, 0.f, 255.f);
            break;
        }
        m_heights[idx] = (int16_t)hitY;
        uint8_t* px = &m_terrain[idx * 4];
        px[0] = rgb[0]; px[1] = rgb[1]; px[2] = rgb[2]; px[3] = 255;
    }
    m_sampleCursor = (m_sampleCursor + perTick) % (GRID * GRID);

    if (m_sampleCursor < perTick) {   // finished a full pass -> upload texture
        std::array<uint8_t, GRID * GRID * 4> copy;
        { std::lock_guard<std::mutex> l(m_mutex); copy = m_terrain; }
        pl::modmenu::registerImage("mm_terrain", copy, GRID, GRID);
    }
}

// ---- waypoints ------------------------------------------------------------------
void MiniMapModule::addWaypointHere() {
    std::lock_guard<std::mutex> l(m_mutex);
    if (!m_haveSelf) return;
    m_waypoints.push_back({"WP" + std::to_string(m_waypoints.size() + 1), m_selfX, m_selfY, m_selfZ});
}
void MiniMapModule::removeNearestWaypoint() {
    std::lock_guard<std::mutex> l(m_mutex);
    if (m_waypoints.empty() || !m_haveSelf) return;
    size_t best = 0; float bd = 1e30f;
    for (size_t i = 0; i < m_waypoints.size(); ++i) {
        float dx = m_waypoints[i].x - m_selfX, dz = m_waypoints[i].z - m_selfZ, d = dx * dx + dz * dz;
        if (d < bd) { bd = d; best = i; }
    }
    m_waypoints.erase(m_waypoints.begin() + best);
}

void MiniMapModule::toggleMap() { m_expandedMap = !m_expandedMap; m_sampleCursor = 0; }
void MiniMapModule::zoomBy(float f) { m_zoom = std::clamp(m_zoom * f, 32.f, 400.f); m_sampleCursor = 0; }
void MiniMapModule::shiftY(int d) { m_underground = true; m_yLevel = std::clamp(m_yLevel + d, -64.f, 319.f); m_sampleCursor = 0; }

// One place for every action so menu buttons and keybinds behave the same.
bool MiniMapModule::handleAction(std::string_view key) {
    if (key == "m_addWaypointButton")    { addWaypointHere();       return true; }
    if (key == "m_removeWaypointButton") { removeNearestWaypoint(); return true; }
    if (key == "m_openMapButton"  || key == "m_openMapKeybind" || key == "keybind") { toggleMap();      return true; }
    if (key == "m_zoomInButton"   || key == "m_zoomInKeybind")   { zoomBy(0.8f);   return true; }
    if (key == "m_zoomOutButton"  || key == "m_zoomOutKeybind")  { zoomBy(1.25f);  return true; }
    if (key == "m_yUpButton"      || key == "m_yUpKeybind")      { shiftY(4);      return true; }
    if (key == "m_yDownButton"    || key == "m_yDownKeybind")    { shiftY(-4);     return true; }
    if (key == "m_surfaceButton") { m_underground = false; m_sampleCursor = 0;     return true; }
    return false;
}

bool MiniMapModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (value != "true" && value != "1") return false;   // only button presses
    return handleAction(key);
}

// Main "keybind" opens/closes the map (instead of disabling the module).
void MiniMapModule::onKeybindEvent(const std::string& key, bool isDown) {
    if (isDown) handleAction(key);
}

// ---- drawing (render thread) ----------------------------------------------------
void MiniMapModule::onFrame() {
    if (!enabled) return;
    std::vector<Blip> blips; std::vector<Waypoint> wps;
    float sx, sy, sz, yaw;
    { std::lock_guard<std::mutex> l(m_mutex);
      if (!m_haveSelf) return;
      blips = m_blips; wps = m_waypoints; sx = m_selfX; sy = m_selfY; sz = m_selfZ; yaw = m_selfYaw; }

    const float size = m_expandedMap ? m_size * m_expandedScale : m_size;
    const float x = m_expandedMap ? -20000.f - size / 2.f : hudPosX;   // expanded: centred like Tablist
    const float y = m_expandedMap ? 40.f : hudPosY;
    const float ppb = size / m_zoom;
    const uint32_t A = (uint32_t)(m_opacity * 255) << 24;
    const float head = m_expandedMap ? 26.f : 18.f;

    std::vector<PLModMenu_DrawCommand> cmds;
    auto add = [&](PLModMenu_DrawCommandType t, float cx, float cy, float w, float h, uint32_t col) -> PLModMenu_DrawCommand& {
        PLModMenu_DrawCommand c = {}; c.type = t; c.x = cx; c.y = cy; c.w = w; c.h = h; c.color = col;
        cmds.push_back(c); return cmds.back();
    };
    auto text = [&](const std::string& s, float tx, float ty, float sz2, uint32_t col) {
        auto& c = add(PL_DRAW_TEXT, tx, ty, 0.f, sz2, col); c.size = sz2; c.text = s;
    };

    add(PL_DRAW_RECT_FILLED, x - 3, y - 3, size + 6, size + 6, 0xFF202020);
    { auto& t = add(PL_DRAW_IMAGE, x, y, size, size, 0x00FFFFFF | A); t.imageId = "mm_terrain"; }

    auto toScreen = [&](float wx, float wz, float& px, float& py) {
        px = x + size / 2 + (wx - sx) * ppb; py = y + size / 2 + (wz - sz) * ppb;
        return px > x && px < x + size && py > y && py < y + size;
    };

    if (m_showWaypoints)
        for (auto& w : wps) {
            float px, py; if (!toScreen(w.x, w.z, px, py)) continue;
            add(PL_DRAW_RECT_FILLED, px - 4, py - 4, 8, 8, 0xFF55FF55);
            if (m_expandedMap) text(w.name, px + 7, py - 6, 14.f, 0xFFFFFFFF);
        }

    for (auto& b : blips) {
        if (b.isPlayer ? !m_showPlayers : !m_showMobs) continue;
        if (m_underground && std::fabs(b.y - m_yLevel) > m_yRange) continue;
        float px, py; if (!toScreen(b.x, b.z, px, py)) continue;
        float hx = px - head / 2, hy = py - head / 2;
        { auto& i = add(PL_DRAW_IMAGE, hx, hy, head, head, 0xFFFFFFFF); i.imageId = b.imageId; }
        if (b.isPlayer && b.sneaking && m_showSneaking) {
            auto& r = add(PL_DRAW_RECT, hx - 2, hy - 2, head + 4, head + 4, 0xFFFFA000); r.size = 2.f;
            text("S", hx + head + 4, hy, 14.f, 0xFFFFA000);
        }
        if (m_showNames && (b.isPlayer ? (m_expandedMap || b.sneaking) : m_expandedMap)) text(b.name, hx, hy + head + 1, 12.f, 0xFFFFFFFF);
    }

    // you: triangle pointing the way you face
    { float cx = x + size / 2, cy = y + size / 2, a = (yaw + 180.f) * 3.14159265f / 180.f;
      auto pt = [&](float ang, float r, float& ox, float& oy) { ox = cx + std::sin(ang) * r; oy = cy - std::cos(ang) * r; };
      float x1, y1, x2, y2, x3, y3; pt(a, 8, x1, y1); pt(a + 2.5f, 6, x2, y2); pt(a - 2.5f, 6, x3, y3);
      auto& t = add(PL_DRAW_TRIANGLE_FILLED, x1, y1, x2, y2, 0xFF40FF40); t.x3 = x3; t.y3 = y3; }

    add(PL_DRAW_RECT, x, y, size, size, 0xFFAAAAAA).size = 2.f;
    char buf[64];
    std::snprintf(buf, sizeof buf, m_underground ? "Y=%d" : "Surface", (int)m_yLevel);
    text(buf, x + 4, y + 4, 12.f, 0xFFFFFFFF);
    std::snprintf(buf, sizeof buf, "%d, %d, %d", (int)sx, (int)sy, (int)sz);
    text(buf, x + 4, y + size - 14, 12.f, 0xFFFFFFFF);

    ::submitDrawCommands(moduleId, cmds);
}

// ---- config ---------------------------------------------------------------------
void MiniMapModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    auto f = [&](const char* k, float& v, float lo, float hi) { if (j.contains(k)) v = std::clamp(j[k].get<float>(), lo, hi); };
    auto b = [&](const char* k, bool& v) { if (j.contains(k)) v = j[k].get<bool>(); };
    f("hudPosX", hudPosX, 0.f, 4000.f); f("hudPosY", hudPosY, 0.f, 4000.f);
    f("m_size", m_size, 80.f, 320.f);   f("m_zoom", m_zoom, 32.f, 400.f);
    f("m_opacity", m_opacity, 0.2f, 1.f);
    f("m_yLevel", m_yLevel, -64.f, 319.f); f("m_yRange", m_yRange, 2.f, 64.f);
    b("m_expandedMap", m_expandedMap); b("m_underground", m_underground);
    b("m_showMobs", m_showMobs); b("m_showPlayers", m_showPlayers);
    auto k = [&](const char* key, int& v) { if (j.contains(key)) v = j[key].get<int>(); };
    k("m_openMapKeybind", m_openMapKeybind); k("m_zoomInKeybind", m_zoomInKeybind); k("m_zoomOutKeybind", m_zoomOutKeybind);
    k("m_yUpKeybind", m_yUpKeybind); k("m_yDownKeybind", m_yDownKeybind);
    b("m_showWaypoints", m_showWaypoints); b("m_showSneaking", m_showSneaking); b("m_showNames", m_showNames);
    if (j.contains("waypoints") && j["waypoints"].is_array()) {
        std::lock_guard<std::mutex> l(m_mutex); m_waypoints.clear();
        for (auto& w : j["waypoints"])
            m_waypoints.push_back({w.value("n", "WP"), w.value("x", 0.f), w.value("y", 0.f), w.value("z", 0.f)});
    }
    m_sampleCursor = 0;
}

void MiniMapModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"] = hudPosX; j["hudPosY"] = hudPosY; j["isHudModule"] = true;
    j["m_size"] = m_size; j["m_zoom"] = m_zoom; j["m_opacity"] = m_opacity;
    j["m_expandedMap"] = m_expandedMap; j["m_underground"] = m_underground;
    j["m_yLevel"] = m_yLevel; j["m_yRange"] = m_yRange;
    j["m_showMobs"] = m_showMobs; j["m_showPlayers"] = m_showPlayers;
    j["m_showWaypoints"] = m_showWaypoints; j["m_showSneaking"] = m_showSneaking; j["m_showNames"] = m_showNames;
    j["m_openMapKeybind"] = m_openMapKeybind; j["m_zoomInKeybind"] = m_zoomInKeybind; j["m_zoomOutKeybind"] = m_zoomOutKeybind;
    j["m_yUpKeybind"] = m_yUpKeybind; j["m_yDownKeybind"] = m_yDownKeybind;
    j["m_openMapButton"] = false; j["m_zoomInButton"] = false; j["m_zoomOutButton"] = false;
    j["m_yUpButton"] = false; j["m_yDownButton"] = false; j["m_surfaceButton"] = false;
    j["m_addWaypointButton"] = false;      // "button" in the key -> shown as a button in the menu
    j["m_removeWaypointButton"] = false;
    std::lock_guard<std::mutex> l(m_mutex);
    auto arr = nlohmann::json::array();
    for (auto& w : m_waypoints) arr.push_back({{"n", w.name}, {"x", w.x}, {"y", w.y}, {"z", w.z}});
    j["waypoints"] = arr;
}
