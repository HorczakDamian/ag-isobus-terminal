#pragma once

#include "CustomVTServer.hpp"
#include "imgui.h"

#include <memory>
#include <string>

namespace isobus_vt
{
    class CanManager;
    class TaskControllerManager;

    struct TerminalScreenBounds
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float maskX = 0.0f;
        float maskY = 0.0f;
        float maskW = 0.0f;
        float maskH = 0.0f;
        bool valid = false;
    };

    class VTRenderer
    {
    public:
        VTRenderer(std::shared_ptr<CustomVTServer> vtServer);
        ~VTRenderer() { save_settings(); }

        void render_terminal_window(bool embedded = false, ImVec2 pos = ImVec2(0, 0), ImVec2 size = ImVec2(0, 0));
        void render_mini_views_window(bool *p_open = nullptr);
        void reset_layout(float posX = -1.0f, float posY = -1.0f, float width = -1.0f, float height = -1.0f)
        {
            resetLayoutRequested = true;
            customPosX = posX;
            customPosY = posY;
            customWidth = width;
            customHeight = height;
        }
        void open_settings_dialog() { showMainSettingsDialog = true; }
        void trigger_soft_key_index(int keyIdx, bool pressed);
        void trigger_virtual_touch(float relX, float relY, bool down);
        TerminalScreenBounds get_terminal_screen_bounds() const { return terminalBounds; }
        void set_managers(std::shared_ptr<CanManager> canMgr, std::shared_ptr<TaskControllerManager> tcMgr)
        {
            canManager = canMgr;
            tcManager = tcMgr;
        }

    private:
        TerminalScreenBounds terminalBounds;
        std::shared_ptr<CustomVTServer> vtServer;
        std::shared_ptr<CanManager> canManager = nullptr;
        std::shared_ptr<TaskControllerManager> tcManager = nullptr;
        bool resetLayoutRequested = false;
        float customPosX = -1.0f;
        float customPosY = -1.0f;
        float customWidth = -1.0f;
        float customHeight = -1.0f;

        // Modal input dialogs state
        bool showNumberKeypad = false;
        std::uint16_t editingNumberObjectId = 0;
        int editingNumberValue = 0;
        int editingNumberMin = 0;
        int editingNumberMax = 100000;

        bool showStringEditor = false;
        std::uint16_t editingStringObjectId = 0;
        char editingStringBuffer[128] = {0};

        bool showInputListPopup = false;
        std::uint16_t editingListObjectId = isobus::NULL_OBJECT_ID;

        // Button press tracking state (AgIsoVT architecture)
        std::shared_ptr<isobus::VTObject> activePressedButton = nullptr;
        std::uint16_t activePressedButtonMaskId = isobus::NULL_OBJECT_ID;
        int activePressedSoftKeyIndex = -1;

        // Settings dialogs state
        bool showMainSettingsDialog   = false;
        bool showVTCapabilitiesDialog = false;
        bool showLanguageDialog       = false;
        bool settingsSavedNotification = false;
        float settingsSavedTimer      = 0.0f;
        float maskScale               = 1.0f;

        // Language/Units dialog state
        char langCode[3]    = {'e', 'n', '\0'};
        char countryCode[3] = {'U', 'S', '\0'};
        int unitArea        = 0;
        int unitDate        = 0;
        int unitDecimal     = 0;
        int unitDistance    = 0;
        int unitForce       = 0;
        int unitGeneric     = 0;
        int unitMass        = 0;
        int unitPressure    = 0;
        int unitTemperature = 0;
        int unitTime        = 0;
        int unitVolume      = 0;

        void render_data_mask(ImDrawList *drawList, ImVec2 maskPos, ImVec2 maskSize,
                             std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws,
                             std::uint16_t maskId,
                             bool interactive = true);

        void render_object(ImDrawList *drawList, ImVec2 parentPos,
                           std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws,
                           std::shared_ptr<isobus::VTObject> obj,
                           std::int16_t relX, std::int16_t relY,
                           std::uint16_t parentId,
                           float scale = 1.0f);

        void render_soft_keys(ImVec2 barPos, ImVec2 barSize,
                             std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws,
                             std::uint16_t softKeyMaskId);

        ImU32 get_imgui_color(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws,
                              std::uint8_t colorIndex,
                              ImU32 defaultColor = IM_COL32(200, 200, 200, 255));

        void render_modals();
        void render_settings_dialogs();

        static bool object_can_be_clicked(std::shared_ptr<isobus::VTObject> object);
        static std::shared_ptr<isobus::VTObject> get_clicked_child_recursive(
            std::shared_ptr<isobus::VTObject> object,
            int x, int y,
            std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws);

    public:
        void load_settings();
        void save_settings();

        // Static colour helper — public so static free functions in .cpp can call it
        static ImU32 get_imgui_color_static(
            std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws,
            std::uint8_t colorIndex);
    };
}
