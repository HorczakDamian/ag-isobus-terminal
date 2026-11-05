#include "ObjectPoolInspector.hpp"
#include "CustomVTServer.hpp"
#include "imgui.h"

#include <algorithm>
#include <sstream>
#include <iomanip>

namespace isobus_vt
{
    ObjectPoolInspector& ObjectPoolInspector::get_instance()
    {
        static ObjectPoolInspector instance;
        return instance;
    }

    std::string ObjectPoolInspector::get_object_type_name(isobus::VirtualTerminalObjectType type)
    {
        switch (type)
        {
            case isobus::VirtualTerminalObjectType::WorkingSet: return "WorkingSet (Root)";
            case isobus::VirtualTerminalObjectType::DataMask: return "DataMask";
            case isobus::VirtualTerminalObjectType::AlarmMask: return "AlarmMask";
            case isobus::VirtualTerminalObjectType::Container: return "Container";
            case isobus::VirtualTerminalObjectType::SoftKeyMask: return "SoftKeyMask";
            case isobus::VirtualTerminalObjectType::Key: return "Key (SoftKey)";
            case isobus::VirtualTerminalObjectType::Button: return "Button";
            case isobus::VirtualTerminalObjectType::InputBoolean: return "InputBoolean";
            case isobus::VirtualTerminalObjectType::InputString: return "InputString";
            case isobus::VirtualTerminalObjectType::InputNumber: return "InputNumber";
            case isobus::VirtualTerminalObjectType::InputList: return "InputList";
            case isobus::VirtualTerminalObjectType::OutputString: return "OutputString";
            case isobus::VirtualTerminalObjectType::OutputNumber: return "OutputNumber";
            case isobus::VirtualTerminalObjectType::OutputList: return "OutputList";
            case isobus::VirtualTerminalObjectType::OutputLine: return "OutputLine";
            case isobus::VirtualTerminalObjectType::OutputRectangle: return "OutputRectangle";
            case isobus::VirtualTerminalObjectType::OutputEllipse: return "OutputEllipse";
            case isobus::VirtualTerminalObjectType::OutputPolygon: return "OutputPolygon";
            case isobus::VirtualTerminalObjectType::OutputMeter: return "OutputMeter";
            case isobus::VirtualTerminalObjectType::OutputLinearBarGraph: return "OutputLinearBarGraph";
            case isobus::VirtualTerminalObjectType::OutputArchedBarGraph: return "OutputArchedBarGraph";
            case isobus::VirtualTerminalObjectType::PictureGraphic: return "PictureGraphic (Bitmap)";
            case isobus::VirtualTerminalObjectType::NumberVariable: return "NumberVariable";
            case isobus::VirtualTerminalObjectType::StringVariable: return "StringVariable";
            case isobus::VirtualTerminalObjectType::FontAttributes: return "FontAttributes";
            case isobus::VirtualTerminalObjectType::LineAttributes: return "LineAttributes";
            case isobus::VirtualTerminalObjectType::FillAttributes: return "FillAttributes";
            case isobus::VirtualTerminalObjectType::WindowMask: return "WindowMask";
            case isobus::VirtualTerminalObjectType::KeyGroup: return "KeyGroup";
            default: return "Object (Other)";
        }
    }

    std::string ObjectPoolInspector::get_object_icon(isobus::VirtualTerminalObjectType type)
    {
        switch (type)
        {
            case isobus::VirtualTerminalObjectType::WorkingSet: return "🚜";
            case isobus::VirtualTerminalObjectType::DataMask: return "🖥️";
            case isobus::VirtualTerminalObjectType::AlarmMask: return "⚠️";
            case isobus::VirtualTerminalObjectType::Container: return "📦";
            case isobus::VirtualTerminalObjectType::SoftKeyMask: return "⌨️";
            case isobus::VirtualTerminalObjectType::Key: return "🔘";
            case isobus::VirtualTerminalObjectType::Button: return "🔳";
            case isobus::VirtualTerminalObjectType::InputBoolean: return "☑️";
            case isobus::VirtualTerminalObjectType::InputString: return "🔤";
            case isobus::VirtualTerminalObjectType::InputNumber: return "🔢";
            case isobus::VirtualTerminalObjectType::OutputString: return "📝";
            case isobus::VirtualTerminalObjectType::OutputNumber: return "💯";
            case isobus::VirtualTerminalObjectType::PictureGraphic: return "🖼️";
            case isobus::VirtualTerminalObjectType::NumberVariable: return "💲";
            case isobus::VirtualTerminalObjectType::StringVariable: return "💬";
            default: return "📄";
        }
    }

    void ObjectPoolInspector::render_inspector_window(bool *p_open)
    {
        if (p_open && !*p_open) return;

        ImGui::SetNextWindowSizeConstraints(ImVec2(800, 560), ImVec2(1920, 1080));
        ImGui::SetNextWindowSize(ImVec2(920, 640), ImGuiCond_FirstUseEver);

        if (!ImGui::Begin("Live Object Pool Tree Browser & Inspector (ISO 11783-6)", p_open))
        {
            ImGui::End();
            return;
        }

        if (!vtServer)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Brak aktywnego serwera Virtual Terminal.");
            ImGui::End();
            return;
        }

        auto activeWs = vtServer->get_active_ws();
        if (!activeWs)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "Oczekiwanie na Working Set maszyny na magistrali CAN...");
            ImGui::End();
            return;
        }

        const auto &tree = activeWs->get_object_tree();
        std::uint8_t ecuAddr = activeWs->get_control_function() ? activeWs->get_control_function()->get_address() : 0;

        // Top info bar
        ImGui::TextColored(ImVec4(0.3f, 0.85f, 1.0f, 1.0f), "Maszyna: ECU 0x%02X (%u)", ecuAddr, ecuAddr);
        ImGui::SameLine();
        ImGui::Text("| Obiektow w puli: %zu", tree.size());
        ImGui::SameLine();
        ImGui::Text("| Aktywna Maska: #%u", vtServer->get_active_data_mask_id());
        ImGui::SameLine();
        ImGui::Text("| SoftKey Maska: #%u", vtServer->get_active_soft_key_mask_id());

        ImGui::Separator();

        // Search bar
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Filtruj / Szukaj ID:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200.0f);
        ImGui::InputTextWithHint("##obj_search", "ID lub nazwa...", searchFilter, sizeof(searchFilter));

        ImGui::SameLine();
        if (ImGui::Button("Wybierz Maska #1000"))
        {
            selectedObjectId = 1000;
        }
        ImGui::SameLine();
        if (ImGui::Button("Wybierz SoftKey #4000"))
        {
            selectedObjectId = 4000;
        }

        ImGui::Separator();

        // Left pane: Object Tree (45% width), Right pane: Inspector Details (55% width)
        float leftWidth = ImGui::GetContentRegionAvail().x * 0.46f;

        ImGui::BeginChild("TreePane", ImVec2(leftWidth, 0), true);
        render_object_tree(tree);
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("DetailsPane", ImVec2(0, 0), true);
        auto it = tree.find(selectedObjectId);
        if (it != tree.end() && it->second)
        {
            render_object_details(it->second);
        }
        else
        {
            ImGui::TextColored(ImVec4(0.6f, 0.7f, 0.8f, 1.0f), "Wybierz obiekt z drzewa po lewej, aby zbadac jego wlasciwosci.");
            ImGui::Spacing();
            ImGui::TextDisabled("Przegladarka puli pozwala analizowac strukture wgrana do VT z pamieci RAM maszyny.");
        }
        ImGui::EndChild();

        ImGui::End();
    }

    void ObjectPoolInspector::render_object_tree(const std::map<std::uint16_t, std::shared_ptr<isobus::VTObject>> &tree)
    {
        if (tree.empty())
        {
            ImGui::TextDisabled("Pula obiektow jest pusta.");
            return;
        }

        // Group objects by functional category:
        // 1. Working Set & Masks
        // 2. Containers
        // 3. Buttons & Inputs
        // 4. Output Displays
        // 5. Variables & Attributes
        std::vector<std::shared_ptr<isobus::VTObject>> masks;
        std::vector<std::shared_ptr<isobus::VTObject>> containers;
        std::vector<std::shared_ptr<isobus::VTObject>> inputsControls;
        std::vector<std::shared_ptr<isobus::VTObject>> outputs;
        std::vector<std::shared_ptr<isobus::VTObject>> variables;
        std::vector<std::shared_ptr<isobus::VTObject>> others;

        for (const auto &pair : tree)
        {
            const auto &obj = pair.second;
            if (!obj) continue;

            // Search filter
            if (searchFilter[0] != '\0')
            {
                std::string filterStr = searchFilter;
                std::transform(filterStr.begin(), filterStr.end(), filterStr.begin(), ::tolower);
                char idStr[16];
                std::snprintf(idStr, sizeof(idStr), "%u", obj->get_id());
                std::string typeStr = get_object_type_name(obj->get_object_type());
                std::transform(typeStr.begin(), typeStr.end(), typeStr.begin(), ::tolower);

                if (std::string(idStr).find(filterStr) == std::string::npos && typeStr.find(filterStr) == std::string::npos)
                {
                    continue;
                }
            }

            auto t = obj->get_object_type();
            if (t == isobus::VirtualTerminalObjectType::WorkingSet ||
                t == isobus::VirtualTerminalObjectType::DataMask ||
                t == isobus::VirtualTerminalObjectType::AlarmMask ||
                t == isobus::VirtualTerminalObjectType::SoftKeyMask)
            {
                masks.push_back(obj);
            }
            else if (t == isobus::VirtualTerminalObjectType::Container)
            {
                containers.push_back(obj);
            }
            else if (t == isobus::VirtualTerminalObjectType::Button ||
                     t == isobus::VirtualTerminalObjectType::Key ||
                     t == isobus::VirtualTerminalObjectType::InputBoolean ||
                     t == isobus::VirtualTerminalObjectType::InputNumber ||
                     t == isobus::VirtualTerminalObjectType::InputString ||
                     t == isobus::VirtualTerminalObjectType::InputList)
            {
                inputsControls.push_back(obj);
            }
            else if (t == isobus::VirtualTerminalObjectType::OutputString ||
                     t == isobus::VirtualTerminalObjectType::OutputNumber ||
                     t == isobus::VirtualTerminalObjectType::OutputMeter ||
                     t == isobus::VirtualTerminalObjectType::OutputLinearBarGraph ||
                     t == isobus::VirtualTerminalObjectType::OutputArchedBarGraph ||
                     t == isobus::VirtualTerminalObjectType::PictureGraphic)
            {
                outputs.push_back(obj);
            }
            else if (t == isobus::VirtualTerminalObjectType::NumberVariable ||
                     t == isobus::VirtualTerminalObjectType::StringVariable ||
                     t == isobus::VirtualTerminalObjectType::FontAttributes ||
                     t == isobus::VirtualTerminalObjectType::LineAttributes ||
                     t == isobus::VirtualTerminalObjectType::FillAttributes)
            {
                variables.push_back(obj);
            }
            else
            {
                others.push_back(obj);
            }
        }

        auto renderList = [this](const char *categoryTitle, const std::vector<std::shared_ptr<isobus::VTObject>> &list) {
            if (list.empty()) return;
            if (ImGui::TreeNodeEx(categoryTitle, ImGuiTreeNodeFlags_DefaultOpen))
            {
                for (const auto &obj : list)
                {
                    bool isSelected = (selectedObjectId == obj->get_id());
                    std::string icon = get_object_icon(obj->get_object_type());
                    std::string typeName = get_object_type_name(obj->get_object_type());
                    char label[128];
                    std::snprintf(label, sizeof(label), "%s #%u: %s", icon.c_str(), obj->get_id(), typeName.c_str());

                    if (ImGui::Selectable(label, isSelected))
                    {
                        selectedObjectId = obj->get_id();
                    }
                }
                ImGui::TreePop();
            }
        };

        renderList("Maski Główne i Klawisze", masks);
        renderList("Kontenery (Grupy)", containers);
        renderList("Przyciski i Wprowadzanie Danych", inputsControls);
        renderList("Wyswietlanie i Grafika", outputs);
        renderList("Zmienne i Atrybuty", variables);
        renderList("Pozostale Obiekty", others);
    }

    void ObjectPoolInspector::render_object_details(std::shared_ptr<isobus::VTObject> obj)
    {
        if (!obj) return;

        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "Szczegoly Obiektu: #%u", obj->get_id());
        ImGui::SameLine();
        ImGui::TextDisabled("(0x%04X)", obj->get_id());

        ImGui::Separator();

        // Main Properties Table
        if (ImGui::BeginTable("ObjPropsTable", 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg))
        {
            ImGui::TableSetupColumn("Wlasciwosc", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            ImGui::TableSetupColumn("Wartosc", ImGuiTableColumnFlags_WidthStretch);

            auto addRow = [](const char *name, const std::string &val) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(name);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(val.c_str());
            };

            addRow("Typ Obiektu (ISO)", get_object_type_name(obj->get_object_type()));
            addRow("ID Obiektu", std::to_string(obj->get_id()));
            addRow("Szerokosc (Width)", std::to_string(obj->get_width()) + " px");
            addRow("Wysokosc (Height)", std::to_string(obj->get_height()) + " px");
            addRow("Kolor tla (Index)", std::to_string(obj->get_background_color()));
            addRow("Liczba podobiektow", std::to_string(obj->get_number_children()));

            ImGui::EndTable();
        }

        // Sub-children list
        if (obj->get_number_children() > 0)
        {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Zawarte podobiekty (Dzieci):");
            if (ImGui::BeginTable("ChildrenTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 50.0f);
                ImGui::TableSetupColumn("Child ID", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                ImGui::TableSetupColumn("Offset X / Y", ImGuiTableColumnFlags_WidthFixed, 120.0f);
                ImGui::TableSetupColumn("Nawigacja", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                for (std::uint16_t i = 0; i < obj->get_number_children(); ++i)
                {
                    std::uint16_t chId = obj->get_child_id(i);
                    std::int16_t chX = obj->get_child_x(i);
                    std::int16_t chY = obj->get_child_y(i);

                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); ImGui::Text("#%u", i);
                    ImGui::TableNextColumn(); ImGui::Text("#%u", chId);
                    ImGui::TableNextColumn(); ImGui::Text("X: %d, Y: %d", chX, chY);
                    ImGui::TableNextColumn();
                    ImGui::PushID(i * 100 + chId);
                    if (ImGui::SmallButton("Przejdz do obiektu ->"))
                    {
                        selectedObjectId = chId;
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
        }
    }
}
