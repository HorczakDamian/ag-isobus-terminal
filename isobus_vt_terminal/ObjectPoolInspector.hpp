#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <map>

#include "isobus/isobus/isobus_virtual_terminal_objects.hpp"

namespace isobus_vt
{
    class CustomVTServer;

    class ObjectPoolInspector
    {
    public:
        static ObjectPoolInspector& get_instance();

        void set_vt_server(std::shared_ptr<CustomVTServer> server) { vtServer = server; }

        void render_inspector_window(bool *p_open);

        // Selection
        void select_object(std::uint16_t objectId) { selectedObjectId = objectId; }
        std::uint16_t get_selected_object_id() const { return selectedObjectId; }

    private:
        ObjectPoolInspector() = default;
        ~ObjectPoolInspector() = default;

        void render_object_tree(const std::map<std::uint16_t, std::shared_ptr<isobus::VTObject>> &tree);
        void render_object_details(std::shared_ptr<isobus::VTObject> obj);

        std::string get_object_type_name(isobus::VirtualTerminalObjectType type);
        std::string get_object_icon(isobus::VirtualTerminalObjectType type);

        std::shared_ptr<CustomVTServer> vtServer = nullptr;
        std::uint16_t selectedObjectId = 0xFFFF; // NULL_OBJECT_ID
        char searchFilter[64] = { 0 };
    };
}
