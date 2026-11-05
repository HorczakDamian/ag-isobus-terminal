#include "DdiExplorerManager.hpp"
#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace isobus_vt
{
    static std::uint32_t get_now_ms()
    {
        return static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    DdiExplorerManager& DdiExplorerManager::get_instance()
    {
        static DdiExplorerManager instance;
        return instance;
    }

    DdiExplorerManager::DdiExplorerManager()
    {
        init_standard_dictionary();
    }

    void DdiExplorerManager::init_standard_dictionary()
    {
        // ISO 11783-11 Data Dictionary Definitions
        auto addDef = [this](std::uint16_t ddi, const std::string &name, const std::string &unit, float res, std::int32_t off, const std::string &cat) {
            dictionary[ddi] = { ddi, name, unit, res, off, cat };
        };

        addDef(0x0001, "Setpoint Volume Per Area (Dawka zadana)", "l/m2", 0.0000001f, 0, "Rate");
        addDef(0x0002, "Actual Volume Per Area (Dawka rzeczywista)", "l/m2", 0.0000001f, 0, "Rate");
        addDef(0x0006, "Setpoint Mass Per Area (Masa zadana)", "mg/m2", 0.001f, 0, "Rate");
        addDef(0x0007, "Actual Mass Per Area (Masa rzeczywista)", "mg/m2", 0.001f, 0, "Rate");
        addDef(0x000B, "Setpoint Count Per Area (Obsada zadana)", "szt/m2", 0.001f, 0, "Rate");
        addDef(0x000C, "Actual Count Per Area (Obsada rzeczywista)", "szt/m2", 0.001f, 0, "Rate");
        addDef(0x0010, "Setpoint Spacing (Odstep nasion zadany)", "mm", 1.0f, 0, "Rate");
        addDef(0x0011, "Actual Spacing (Odstep nasion rzeczywisty)", "mm", 1.0f, 0, "Rate");

        addDef(0x0042, "Actual Working Width (Szerokosc robocza)", "mm", 1.0f, 0, "Width");
        addDef(0x0043, "Setpoint Working Width (Szerokosc zadana)", "mm", 1.0f, 0, "Width");

        addDef(0x0051, "Total Area (Powierzchnia calkowita)", "m2", 1.0f, 0, "Totals");
        addDef(0x0052, "Effective Total Distance (Dystans efektywny)", "m", 1.0f, 0, "Totals");
        addDef(0x0053, "Effective Total Time (Czas efektywny)", "s", 1.0f, 0, "Totals");
        addDef(0x0057, "Total Mass (Masa calkowita nawozu)", "mg", 0.000001f, 0, "Totals");
        addDef(0x0058, "Total Volume (Objetosc calkowita cieczy)", "ml", 0.001f, 0, "Totals");

        addDef(0x008D, "Actual Work State (Stan pracy narzedzia)", "bit", 1.0f, 0, "Section");
        addDef(0x008E, "Setpoint Work State (Stan zadany narzedzia)", "bit", 1.0f, 0, "Section");
        addDef(0x00A1, "Actual Condensed Work State 1-16 (Sekcje 1-16)", "bitfield", 1.0f, 0, "Section");
        addDef(0x00A2, "Actual Condensed Work State 17-32 (Sekcje 17-32)", "bitfield", 1.0f, 0, "Section");
        addDef(0x00A3, "Actual Condensed Work State 33-48 (Sekcje 33-48)", "bitfield", 1.0f, 0, "Section");
        addDef(0x00A4, "Actual Condensed Work State 49-64 (Sekcje 49-64)", "bitfield", 1.0f, 0, "Section");
        addDef(0x0122, "Setpoint Condensed Work State 1-16 (Zadane Sekcje 1-16)", "bitfield", 1.0f, 0, "Section");

        addDef(0x0090, "Actual Ground Based Speed (Predkosc z radaru)", "mm/s", 0.001f, 0, "Speed");
        addDef(0x0091, "Actual Wheel Based Speed (Predkosc kol)", "mm/s", 0.001f, 0, "Speed");

        addDef(0x0094, "Actual Liquid Pressure (Cisnienie robocze cieczy)", "kPa", 0.001f, 0, "Pressure");
        addDef(0x0095, "Actual Boom Pressure (Cisnienie belki opryskiwacza)", "kPa", 0.001f, 0, "Pressure");

        addDef(0x009B, "Actual Tank Level (Poziom cieczy w zbiorniku)", "l", 0.001f, 0, "Tank");
        addDef(0x009C, "Actual Hopper Level (Poziom w skrzyni nasiennej)", "l", 0.001f, 0, "Tank");

        addDef(0x00AF, "Actual Fan RPM (Obroty dmuchawy siewnika)", "RPM", 0.001f, 0, "Misc");
        addDef(0x00B0, "Actual Shaft RPM (Obroty walka aparatu)", "RPM", 0.001f, 0, "Misc");
    }

    DdiDefinition DdiExplorerManager::lookup_definition(std::uint16_t ddi)
    {
        auto it = dictionary.find(ddi);
        if (it != dictionary.end()) return it->second;

        // Fallback for unknown/proprietary DDI
        char hexBuf[16];
        std::snprintf(hexBuf, sizeof(hexBuf), "0x%04X", ddi);
        return { ddi, "DDI " + std::string(hexBuf) + " (Niezdefiniowany)", "raw", 1.0f, 0, "Misc" };
    }

    void DdiExplorerManager::update_ddi_value(std::uint16_t ddi, std::int32_t rawValue, std::uint8_t sa, std::uint16_t element)
    {
        std::lock_guard<std::mutex> lock(dataMutex);
        auto it = liveRecords.find(ddi);
        if (it == liveRecords.end())
        {
            DdiLiveRecord rec;
            rec.def = lookup_definition(ddi);
            rec.rawValue = rawValue;
            rec.scaledValue = static_cast<float>(rawValue) * rec.def.resolution + rec.def.offset;
            rec.lastUpdateTimestampMs = get_now_ms();
            rec.updateCount = 1;
            rec.minValue = rec.scaledValue;
            rec.maxValue = rec.scaledValue;
            rec.sumValues = rec.scaledValue;
            rec.history.push_back(rec.scaledValue);
            rec.sourceAddress = sa;
            rec.elementNumber = element;
            liveRecords[ddi] = rec;
        }
        else
        {
            auto &rec = it->second;
            rec.rawValue = rawValue;
            rec.scaledValue = static_cast<float>(rawValue) * rec.def.resolution + rec.def.offset;
            rec.lastUpdateTimestampMs = get_now_ms();
            rec.updateCount++;
            rec.minValue = std::min(rec.minValue, rec.scaledValue);
            rec.maxValue = std::max(rec.maxValue, rec.scaledValue);
            rec.sumValues += rec.scaledValue;
            rec.history.push_back(rec.scaledValue);
            if (rec.history.size() > 60) rec.history.pop_front();
            rec.sourceAddress = sa;
            rec.elementNumber = element;
        }
    }

    void DdiExplorerManager::record_process_data_from_can(std::uint32_t pgn, std::uint8_t sa, const std::uint8_t *data, std::uint32_t len)
    {
        if (!explorerEnabled) return;
        // ISO 11783-10 Process Data PGN 65098 (0xFE4A) or TC PGN (0xCB00)
        if (pgn != 0xCB00 && pgn != 0xFE4A) return;
        if (!data || len < 6) return;

        // Process data can contain [DDI_LSB, DDI_MSB, Element_LSB, Element_MSB, Val_B0, Val_B1, Val_B2, Val_B3]
        std::uint16_t ddi = static_cast<std::uint16_t>(data[0]) | (static_cast<std::uint16_t>(data[1]) << 8);
        std::uint16_t elem = static_cast<std::uint16_t>(data[2]) | (static_cast<std::uint16_t>(data[3]) << 8);
        std::int32_t val = 0;
        if (len >= 8)
        {
            val = static_cast<std::int32_t>(
                static_cast<std::uint32_t>(data[4]) |
                (static_cast<std::uint32_t>(data[5]) << 8) |
                (static_cast<std::uint32_t>(data[6]) << 16) |
                (static_cast<std::uint32_t>(data[7]) << 24));
        }
        else
        {
            val = static_cast<std::int32_t>(static_cast<std::uint32_t>(data[4]) | (static_cast<std::uint32_t>(data[5]) << 8));
        }

        update_ddi_value(ddi, val, sa, elem);
    }

    void DdiExplorerManager::inject_sample_ddis()
    {
        update_ddi_value(0x0001, 2200000, 0x81, 1); // 220 l/ha
        update_ddi_value(0x0002, 2185000, 0x81, 1); // 218.5 l/ha
        update_ddi_value(0x0042, 24000, 0x81, 1);   // 24.0 m
        update_ddi_value(0x0090, 2222, 0x81, 1);    // 8.0 km/h (2222 mm/s)
        update_ddi_value(0x0094, 3200, 0x81, 1);    // 3.2 bar
        update_ddi_value(0x009B, 1850, 0x81, 1);    // 1850 l in tank
        update_ddi_value(0x00A1, 0x0055, 0x81, 1);  // Sections 1..4 on
    }

    void DdiExplorerManager::render_ddi_explorer_window(bool *p_open)
    {
        if (!p_open || !*p_open) return;

        ImGui::SetNextWindowSizeConstraints(ImVec2(720, 520), ImVec2(1920, 1080));
        ImGui::SetNextWindowSize(ImVec2(850, 600), ImGuiCond_FirstUseEver);

        if (!ImGui::Begin("DDI Process Data Explorer (ISO 11783-11)", p_open))
        {
            ImGui::End();
            return;
        }

        std::lock_guard<std::mutex> lock(dataMutex);

        // Header and quick stats
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.0f, 1.0f), "ISOBUS Data Dictionary (ISO 11783-11) & Process Data Monitor");
        ImGui::SameLine();
        ImGui::Checkbox("Włączony", &explorerEnabled);
        ImGui::SameLine(ImGui::GetWindowWidth() - 220.0f);
        if (ImGui::Button("🧪 Wstrzyknij dane testowe"))
        {
            inject_sample_ddis();
        }

        ImGui::Separator();

        // Filters
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Szukaj:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputTextWithHint("##ddi_search", "Nazwa lub kod DDI...", searchFilter, sizeof(searchFilter));

        ImGui::SameLine();
        ImGui::Text("Kategoria:");
        ImGui::SameLine();
        const char *categories[] = { "Wszystkie", "Dawka (Rate)", "Szerokosc (Width)", "Sekcje (Section)", "Predkosc (Speed)", "Cisnienie (Pressure)", "Zbiornik (Tank)", "Inne (Misc)" };
        ImGui::SetNextItemWidth(170.0f);
        ImGui::Combo("##cat_combo", &selectedCategoryIdx, categories, IM_ARRAYSIZE(categories));

        ImGui::SameLine();
        ImGui::TextDisabled("| Aktywne DDI: %zu", liveRecords.size());

        ImGui::Separator();

        // Table with Sparklines
        float tableHeight = (selectedDdiForGraph > 0) ? 260.0f : 380.0f;
        if (ImGui::BeginTable("DdiTable", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0, tableHeight)))
        {
            ImGui::TableSetupColumn("DDI (Hex)", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Opis Parametru ISOBUS", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Wartosc Rzeczywista", ImGuiTableColumnFlags_WidthFixed, 130.0f);
            ImGui::TableSetupColumn("Jedn.", ImGuiTableColumnFlags_WidthFixed, 60.0f);
            ImGui::TableSetupColumn("Min / Max", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("Wykres na zywo", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Zrodlo SA", ImGuiTableColumnFlags_WidthFixed, 75.0f);
            ImGui::TableHeadersRow();

            for (const auto &pair : liveRecords)
            {
                const auto &rec = pair.second;

                // Search filtering
                if (searchFilter[0] != '\0')
                {
                    std::string filterStr = searchFilter;
                    std::transform(filterStr.begin(), filterStr.end(), filterStr.begin(), ::tolower);
                    std::string nameLower = rec.def.name;
                    std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
                    char ddiStr[16];
                    std::snprintf(ddiStr, sizeof(ddiStr), "%u", rec.def.ddi);

                    if (nameLower.find(filterStr) == std::string::npos && std::string(ddiStr).find(filterStr) == std::string::npos)
                    {
                        continue;
                    }
                }

                // Category filtering
                if (selectedCategoryIdx > 0)
                {
                    const char *targetCat = (selectedCategoryIdx == 1) ? "Rate" :
                                            (selectedCategoryIdx == 2) ? "Width" :
                                            (selectedCategoryIdx == 3) ? "Section" :
                                            (selectedCategoryIdx == 4) ? "Speed" :
                                            (selectedCategoryIdx == 5) ? "Pressure" :
                                            (selectedCategoryIdx == 6) ? "Tank" : "Misc";
                    if (rec.def.category != targetCat) continue;
                }

                ImGui::TableNextRow();

                // 1. DDI (Hex / Dec)
                ImGui::TableNextColumn();
                bool isSelected = (selectedDdiForGraph == rec.def.ddi);
                char ddiLabel[32];
                std::snprintf(ddiLabel, sizeof(ddiLabel), "0x%04X (%u)", rec.def.ddi, rec.def.ddi);
                if (ImGui::Selectable(ddiLabel, isSelected, ImGuiSelectableFlags_SpanAllColumns))
                {
                    selectedDdiForGraph = rec.def.ddi;
                }

                // 2. Name
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(rec.def.name.c_str());

                // 3. Value
                ImGui::TableNextColumn();
                if (rec.def.category == "Rate" && rec.def.unit == "l/m2")
                {
                    // Convert l/m2 to l/ha (multiply by 10,000)
                    float lPerHa = rec.scaledValue * 10000.0f;
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%.1f l/ha", lPerHa);
                }
                else if (rec.def.category == "Width" && rec.def.unit == "mm")
                {
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%.2f m", rec.scaledValue / 1000.0f);
                }
                else if (rec.def.category == "Speed" && rec.def.unit == "mm/s")
                {
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%.1f km/h", (rec.scaledValue * 3.6f));
                }
                else
                {
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%.2f", rec.scaledValue);
                }

                // 4. Units
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", rec.def.unit.c_str());

                // 5. Min / Max
                ImGui::TableNextColumn();
                ImGui::Text("%.1f / %.1f", rec.minValue, rec.maxValue);

                // 6. Sparkline
                ImGui::TableNextColumn();
                if (!rec.history.empty())
                {
                    std::vector<float> hist(rec.history.begin(), rec.history.end());
                    ImGui::PushID(rec.def.ddi);
                    ImGui::PlotLines("##spark", hist.data(), static_cast<int>(hist.size()), 0, nullptr, rec.minValue * 0.9f, rec.maxValue * 1.1f, ImVec2(90, 20));
                    ImGui::PopID();
                }

                // 7. Source Address
                ImGui::TableNextColumn();
                ImGui::Text("0x%02X", rec.sourceAddress);
            }

            ImGui::EndTable();
        }

        // Detailed live graph for selected DDI
        if (selectedDdiForGraph > 0 && liveRecords.find(selectedDdiForGraph) != liveRecords.end())
        {
            ImGui::Spacing();
            ImGui::Separator();
            const auto &sel = liveRecords[selectedDdiForGraph];
            ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.3f, 1.0f), "Szczegolowy Wykres Czasowy: DDI 0x%04X - %s", sel.def.ddi, sel.def.name.c_str());

            std::vector<float> graphData(sel.history.begin(), sel.history.end());
            if (!graphData.empty())
            {
                char overlay[64];
                std::snprintf(overlay, sizeof(overlay), "Biezaca: %.2f %s | Min: %.2f | Max: %.2f", sel.scaledValue, sel.def.unit.c_str(), sel.minValue, sel.maxValue);
                ImGui::PlotLines("##detailed_graph", graphData.data(), static_cast<int>(graphData.size()), 0, overlay, sel.minValue * 0.9f, sel.maxValue * 1.1f, ImVec2(-1, 110));
            }
        }

        ImGui::End();
    }
}
