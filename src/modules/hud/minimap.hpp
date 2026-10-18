#pragma once

#include "../Module.hpp"
#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bedrocktools::sdk { class Player; }

class MiniMapModule : public Module {
public:
    MiniMapModule();
    ~MiniMapModule() override;

    void onInit() override;
    void onEnable() override;
    void onDisable() override;
    void onFrame() override;
    bool onMenuConfigChanged(std::string_view key, std::string_view value) override;
    void onKeybindEvent(const std::string& key, bool isDown) override;
    void loadConfig(const nlohmann::json& j) override;
    void saveConfig(nlohmann::json& j) override;

    void onLocalPlayerTick(bedrocktools::sdk::Player* player);

    static MiniMapModule* getInstance();

    // HUD position (movable in the HUD editor)
    float hudPosX = 20.f;
    float hudPosY = 60.f;
    bool isHudModule = true;

private:
    static constexpr int GRID = 64;        // terrain texture is GRID x GRID cells
    static constexpr int HEAD_PX = 32;     // head icon texture size

    struct Blip {
        float x = 0, y = 0, z = 0;
        bool isPlayer = false;
        bool sneaking = false;
        std::string name;
        std::string imageId;
    };
    struct Waypoint { std::string name; float x, y, z; };

    // settings (menu keys)
    float m_size = 160.f;           // map size in px (small mode)
    float m_expandedScale = 2.6f;   // size multiplier when expanded
    float m_zoom = 96.f;            // blocks shown across the map
    float m_opacity = 0.9f;
    bool m_expandedMap = false;
    bool m_underground = false;     // slice terrain at m_yLevel
    float m_yLevel = 40.f;
    float m_yRange = 12.f;          // entity Y filter while underground
    bool m_showMobs = true;
    bool m_showPlayers = true;
    bool m_showWaypoints = true;
    bool m_showSneaking = true;
    bool m_showNames = true;

    // runtime state (guarded by m_mutex)
    std::mutex m_mutex;
    std::vector<Blip> m_blips;
    std::vector<Waypoint> m_waypoints;
    std::array<uint8_t, GRID * GRID * 4> m_terrain{};
    float m_selfX = 0, m_selfY = 0, m_selfZ = 0, m_selfYaw = 0;
    bool m_haveSelf = false;
    int m_tick = 0;
    int m_sampleCursor = 0;
    std::unordered_map<std::string, int> m_headTick;
    std::unordered_set<std::string> m_mobIcons;
    void* m_airBlock = nullptr;
    std::unordered_map<const void*, uint32_t> m_blockColors;   // Block* -> colour (game thread only)
    std::array<int16_t, GRID * GRID> m_heights{};

    void sampleTerrain(bedrocktools::sdk::Player* lp);
    int m_openMapKeybind = 0, m_zoomInKeybind = 0, m_zoomOutKeybind = 0, m_yUpKeybind = 0, m_yDownKeybind = 0;

    void toggleMap();
    void zoomBy(float factor);
    void shiftY(int delta);
    bool handleAction(std::string_view key);
    void addWaypointHere();
    void removeNearestWaypoint();

    static MiniMapModule* s_instance;
};
