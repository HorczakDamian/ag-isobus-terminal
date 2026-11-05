#include "AefPoolLinter.hpp"
#include "CustomVTServer.hpp"
#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>
#include <windows.h>
#include <shellapi.h>

namespace isobus_vt
{
    static std::uint32_t get_now_ms()
    {
        return static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    AefPoolLinter& AefPoolLinter::get_instance()
    {
        static AefPoolLinter instance;
        return instance;
    }

    bool AefPoolLinter::check_cycles_dfs(std::uint16_t currentId,
                                         const std::map<std::uint16_t, std::shared_ptr<isobus::VTObject>> &tree,
                                         std::set<std::uint16_t> &visited,
                                         std::set<std::uint16_t> &recursionStack,
                                         std::vector<std::uint16_t> &cyclePath)
    {
        visited.insert(currentId);
        recursionStack.insert(currentId);
        cyclePath.push_back(currentId);

        auto it = tree.find(currentId);
        if (it != tree.end() && it->second)
        {
            auto obj = it->second;
            for (std::uint16_t i = 0; i < obj->get_number_children(); ++i)
            {
                std::uint16_t childId = obj->get_child_id(i);
                if (childId == 0xFFFF) continue; // NULL_OBJECT_ID

                if (recursionStack.find(childId) != recursionStack.end())
                {
                    // Loop / cycle detected!
                    cyclePath.push_back(childId);
                    return true;
                }

                if (visited.find(childId) == visited.end())
                {
                    if (check_cycles_dfs(childId, tree, visited, recursionStack, cyclePath))
                    {
                        return true;
                    }
                }
            }
        }

        recursionStack.erase(currentId);
        cyclePath.pop_back();
        return false;
    }

    LinterReport AefPoolLinter::run_audit()
    {
        LinterReport report;
        report.scanTimestampMs = get_now_ms();
        hasRunOnce = true;

        if (!vtServer)
        {
            currentReport = report;
            return report;
        }

        auto activeWs = vtServer->get_active_ws();
        if (!activeWs)
        {
            currentReport = report;
            return report;
        }

        const auto &tree = activeWs->get_object_tree();
        report.totalObjects = static_cast<std::uint32_t>(tree.size());
        report.workingSetLabel = "ECU " + std::to_string(activeWs->get_control_function() ? activeWs->get_control_function()->get_address() : 0);

        if (tree.empty())
        {
            report.issues.push_back({
                "AEF-ERR-00",
                LinterSeverity::Error,
                0xFFFF,
                "Pusta pula obiektow",
                "W pamieci roboczej VT nie znaleziono zadnych obiektow ISOBUS.",
                "Upewnij sie, ze sterownik maszyny zakonczyl transfer ETP i wyslal komende EndOfObjectPool."
            });
            report.criticalErrorsCount = 1;
            report.aefScorePct = 0.0f;
            currentReport = report;
            return report;
        }

        // 1. Check WorkingSet Root object
        bool hasWorkingSet = false;
        std::set<std::uint16_t> referencedChildren;
        std::vector<std::uint16_t> dataMasks;

        for (const auto &pair : tree)
        {
            const auto &obj = pair.second;
            if (!obj) continue;

            if (obj->get_object_type() == isobus::VirtualTerminalObjectType::WorkingSet)
            {
                hasWorkingSet = true;
            }
            if (obj->get_object_type() == isobus::VirtualTerminalObjectType::DataMask)
            {
                dataMasks.push_back(obj->get_id());
            }

            for (std::uint16_t i = 0; i < obj->get_number_children(); ++i)
            {
                referencedChildren.insert(obj->get_child_id(i));
            }
        }

        if (!hasWorkingSet)
        {
            report.issues.push_back({
                "AEF-ERR-01",
                LinterSeverity::Error,
                0,
                "Brak obiektu glownego WorkingSet",
                "Kazda pula obiektow ISO 11783-6 musi zawierac co najmniej jeden obiekt typu WorkingSet (Type=0).",
                "Dodaj obiekt WorkingSet definiujacy wlasciwosci calego zestawu maszynowego."
            });
            report.criticalErrorsCount++;
        }

        if (dataMasks.empty())
        {
            report.issues.push_back({
                "AEF-ERR-02",
                LinterSeverity::Error,
                0,
                "Brak masek danych (DataMask)",
                "Pula obiektow nie posiada zdefiniowanej ani jednej maski danych do wyswietlenia.",
                "Zdefiniuj co najmniej jedna glowna maske danych (np. ID #1000)."
            });
            report.criticalErrorsCount++;
        }

        // 2. Dangling references & bounds checks
        for (const auto &pair : tree)
        {
            const auto &obj = pair.second;
            if (!obj) continue;

            // Check children
            for (std::uint16_t i = 0; i < obj->get_number_children(); ++i)
            {
                std::uint16_t chId = obj->get_child_id(i);
                if (chId == 0xFFFF) continue; // NULL_OBJECT_ID is allowed in some slots

                if (tree.find(chId) == tree.end())
                {
                    report.issues.push_back({
                        "AEF-ERR-03",
                        LinterSeverity::Error,
                        obj->get_id(),
                        "Brakujacy podobiekt (Dangling Child ID)",
                        "Obiekt #" + std::to_string(obj->get_id()) + " odwoluje sie do dziecka #" + std::to_string(chId) + ", ktorego nie ma w puli.",
                        "Dodaj brakujacy obiekt #" + std::to_string(chId) + " do definicji puli lub usun odwolanie."
                    });
                    report.criticalErrorsCount++;
                }

                // Coordinate bounds check
                std::int16_t chX = obj->get_child_x(i);
                std::int16_t chY = obj->get_child_y(i);
                if (chX < -50 || chY < -50 || chX > 1000 || chY > 1000)
                {
                    report.issues.push_back({
                        "AEF-WARN-04",
                        LinterSeverity::Warning,
                        obj->get_id(),
                        "Nieprawidlowe wspolrzedne dziecka",
                        "Podobiekt #" + std::to_string(chId) + " w obiekcie #" + std::to_string(obj->get_id()) + " ma nienaturalne wspolrzedne (X=" + std::to_string(chX) + ", Y=" + std::to_string(chY) + ").",
                        "Upewnij sie, ze obiekt miesci sie na ekranie VT (obszar standardowy 480x480 px)."
                    });
                    report.warningsCount++;
                }
            }

            // Zero dimension warnings for visual objects
            auto t = obj->get_object_type();
            if (t == isobus::VirtualTerminalObjectType::Button ||
                t == isobus::VirtualTerminalObjectType::InputNumber ||
                t == isobus::VirtualTerminalObjectType::OutputString)
            {
                if (obj->get_width() == 0 || obj->get_height() == 0)
                {
                    report.issues.push_back({
                        "AEF-WARN-05",
                        LinterSeverity::Warning,
                        obj->get_id(),
                        "Zerowe wymiary kontrolki wizualnej",
                        "Obiekt #" + std::to_string(obj->get_id()) + " ma szerokosc lub wysokosc rowna 0 px, przez co moze byc niewidoczny dla operatora.",
                        "Ustaw dodatnia szerokosc i wysokosc kontrolki."
                    });
                    report.warningsCount++;
                }
            }
        }

        // 3. Cycle Detection
        std::set<std::uint16_t> visited;
        std::set<std::uint16_t> recursionStack;
        std::vector<std::uint16_t> cyclePath;

        for (const auto &dmId : dataMasks)
        {
            cyclePath.clear();
            if (check_cycles_dfs(dmId, tree, visited, recursionStack, cyclePath))
            {
                std::string pathStr;
                for (size_t i = 0; i < cyclePath.size(); ++i)
                {
                    pathStr += "#" + std::to_string(cyclePath[i]);
                    if (i + 1 < cyclePath.size()) pathStr += " -> ";
                }

                report.issues.push_back({
                    "AEF-CRIT-06",
                    LinterSeverity::Error,
                    cyclePath.front(),
                    "KRYTYCZNY: Zapetlenie obiektow (Circular Reference)",
                    "Wykryto zapetlenie relacji rodzic-dziecko w kontenerach: " + pathStr + ". Taka struktura zawiesza komercyjne terminale VT!",
                    "Usun zapetlone odwolanie w kontenerze."
                });
                report.criticalErrorsCount++;
                break;
            }
        }

        // 4. Orphaned objects check
        for (const auto &pair : tree)
        {
            std::uint16_t objId = pair.first;
            auto t = pair.second->get_object_type();

            // Root masks are not orphans
            if (t == isobus::VirtualTerminalObjectType::WorkingSet ||
                t == isobus::VirtualTerminalObjectType::DataMask ||
                t == isobus::VirtualTerminalObjectType::AlarmMask ||
                t == isobus::VirtualTerminalObjectType::SoftKeyMask)
            {
                continue;
            }

            if (referencedChildren.find(objId) == referencedChildren.end())
            {
                report.issues.push_back({
                    "AEF-INFO-07",
                    LinterSeverity::Info,
                    objId,
                    "Obiekt osierocony (Orphaned Object)",
                    "Obiekt #" + std::to_string(objId) + " istnieje w puli, ale zaden kontener ani maska nie odwoluja sie do niego.",
                    "Jesli obiekt nie jest dynamicznie dolaczany makrami, mozna go usunac w celu oszczedzania pamieci VT."
                });
                report.infosCount++;
            }
        }

        // Compute Conformance Score (100% minus penalties for errors & warnings)
        // Wskazowki (Info) sa czysto informacyjne dla programistow maszyny i nie obnizaja zgodnosci
        float score = 100.0f;
        score -= (report.criticalErrorsCount * 25.0f);
        score -= (report.warningsCount * 3.0f);
        if (report.criticalErrorsCount == 0 && report.warningsCount == 0)
        {
            score = 100.0f;
        }
        report.aefScorePct = std::clamp(score, 0.0f, 100.0f);

        currentReport = report;
        return report;
    }

    bool AefPoolLinter::export_report_to_html(const std::string &filePath)
    {
        std::ofstream out(filePath);
        if (!out.is_open()) return false;

        out << "<!DOCTYPE html><html lang='pl'><head><meta charset='UTF-8'>"
            << "<title>Raport Zgodności AEF & ISO 11783-6</title>"
            << "<style>body{font-family:Segoe UI,sans-serif;background:#1a1e24;color:#eee;margin:20px;}"
            << "h1{color:#4fc3f7;}table{border-collapse:collapse;width:100%;margin-top:20px;}"
            << "th,td{border:1px solid #444;padding:8px 12px;text-align:left;}th{background:#2a313d;color:#81d4fa;}"
            << ".err{color:#ff5252;font-weight:bold;}.warn{color:#ffd740;}.info{color:#80d8ff;}"
            << ".badge{padding:4px 8px;border-radius:4px;font-weight:bold;display:inline-block;}"
            << ".badge-score{background:#2e7d32;color:#fff;font-size:16px;}</style></head><body>";

        out << "<h1>Audyt Puli Obiektów ISOBUS — Raport Zgodności AEF</h1>";
        out << "<p>Zbadana maszyna: <b>" << currentReport.workingSetLabel << "</b> | Całkowita liczba obiektów: <b>" << currentReport.totalObjects << "</b></p>";
        out << "<p>Wynik Zgodności AEF: <span class='badge badge-score'>" << static_cast<int>(currentReport.aefScorePct) << "% ZGODNOŚĆ</span></p>";
        out << "<p>Błędy krytyczne: <b class='err'>" << currentReport.criticalErrorsCount << "</b> | Ostrzeżenia: <b class='warn'>" << currentReport.warningsCount << "</b> | Informacje: <b class='info'>" << currentReport.infosCount << "</b></p>";

        out << "<table><thead><tr><th>Kod Reguły</th><th>Poziom</th><th>ID Obiektu</th><th>Tytuł</th><th>Opis Problemu</th><th>Rekomendacja AEF</th></tr></thead><tbody>";

        for (const auto &iss : currentReport.issues)
        {
            const char *sevClass = (iss.severity == LinterSeverity::Error) ? "err" : ((iss.severity == LinterSeverity::Warning) ? "warn" : "info");
            const char *sevText = (iss.severity == LinterSeverity::Error) ? "BŁĄD" : ((iss.severity == LinterSeverity::Warning) ? "OSTRZEŻENIE" : "INFO");

            out << "<tr><td>" << iss.ruleId << "</td>"
                << "<td class='" << sevClass << "'>" << sevText << "</td>"
                << "<td>#" << iss.objectId << "</td>"
                << "<td>" << iss.title << "</td>"
                << "<td>" << iss.description << "</td>"
                << "<td>" << iss.recommendation << "</td></tr>";
        }

        out << "</tbody></table></body></html>";
        return true;
    }

    void AefPoolLinter::render_linter_window(bool *p_open)
    {
        if (p_open && !*p_open) return;

        ImGui::SetNextWindowSizeConstraints(ImVec2(800, 540), ImVec2(1920, 1080));
        ImGui::SetNextWindowSize(ImVec2(920, 620), ImGuiCond_FirstUseEver);

        if (!ImGui::Begin("AEF Object Pool Linter & Conformance Checker", p_open))
        {
            ImGui::End();
            return;
        }

        // Header and Scan Actions
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.0f, 1.0f), "Walidator Zgodnosci Puli Maszyny z Wytycznymi AEF & ISO 11783-6");

        ImGui::SameLine(ImGui::GetWindowWidth() - 360.0f);
        if (ImGui::Button("[ Uruchom Audyt Puli (AEF) ]"))
        {
            run_audit();
        }

        ImGui::SameLine();
        if (ImGui::Button("[ Eksportuj Raport HTML ]"))
        {
            if (export_report_to_html("raport_zgodnosci_aef.html"))
            {
                ShellExecuteA(NULL, "open", "raport_zgodnosci_aef.html", NULL, NULL, SW_SHOWNORMAL);
            }
        }

        ImGui::Separator();

        if (!hasRunOnce)
        {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.7f, 0.8f, 0.9f, 1.0f), "Kliknij przycisk powyzej, aby przeskanowac aktualnie podlaczona maszyne pod katem bledow, zapetlen i wytycznych AEF.");
            ImGui::End();
            return;
        }

        // Score Card & Metrics
        ImVec4 scoreColor = (currentReport.aefScorePct >= 90.0f) ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f) :
                            (currentReport.aefScorePct >= 70.0f) ? ImVec4(1.0f, 0.85f, 0.2f, 1.0f) :
                                                                  ImVec4(1.0f, 0.3f, 0.3f, 1.0f);

        ImGui::Text("AEF Conformance Score:");
        ImGui::SameLine();
        ImGui::TextColored(scoreColor, "%.0f%%", currentReport.aefScorePct);
        ImGui::SameLine(220.0f);
        ImGui::Text("Obiektow: %u", currentReport.totalObjects);
        ImGui::SameLine(340.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Bledy: %u", currentReport.criticalErrorsCount);
        ImGui::SameLine(440.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "Ostrzezenia: %u", currentReport.warningsCount);
        ImGui::SameLine(570.0f);
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Wskazowki: %u", currentReport.infosCount);

        ImGui::ProgressBar(currentReport.aefScorePct / 100.0f, ImVec2(-1, 16.0f), "");

        if (currentReport.criticalErrorsCount == 0 && currentReport.warningsCount == 0)
        {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "[ ZGODNOSC 100%% ]: Pula obiektow jest w pelni poprawna i bezpieczna. Brak bledow i ostrzezen.");
        }
        if (currentReport.infosCount > 0)
        {
            ImGui::TextDisabled("Wskazowki (INFO: %u): Obiekty pomocnicze, podrzedne kontenery lub grafiki uzywane przez maszyne dynamicznie.", currentReport.infosCount);
        }

        ImGui::Separator();

        // Severity filter
        ImGui::AlignTextToFramePadding();
        ImGui::Text("Filtruj powiadomienia:");
        ImGui::SameLine();
        ImGui::RadioButton("Wszystkie", &filterSeverity, -1);
        ImGui::SameLine();
        ImGui::RadioButton("Tylko Bledy", &filterSeverity, 2);
        ImGui::SameLine();
        ImGui::RadioButton("Tylko Ostrzezenia", &filterSeverity, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Tylko Informacje", &filterSeverity, 0);

        // Issues Table
        if (ImGui::BeginTable("LinterIssuesTable", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0, -1)))
        {
            ImGui::TableSetupColumn("Poziom", ImGuiTableColumnFlags_WidthFixed, 85.0f);
            ImGui::TableSetupColumn("Regula", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Obiekt", ImGuiTableColumnFlags_WidthFixed, 75.0f);
            ImGui::TableSetupColumn("Tytul Problemu", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Zalecenie AEF", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (const auto &iss : currentReport.issues)
            {
                if (filterSeverity != -1 && static_cast<int>(iss.severity) != filterSeverity) continue;

                ImGui::TableNextRow();

                // 1. Severity
                ImGui::TableNextColumn();
                if (iss.severity == LinterSeverity::Error)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "[BLAD]");
                }
                else if (iss.severity == LinterSeverity::Warning)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "[OSTRZ.]");
                }
                else
                {
                    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "INFO");
                }

                // 2. Rule
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(iss.ruleId.c_str());

                // 3. Object ID
                ImGui::TableNextColumn();
                if (iss.objectId != 0xFFFF)
                {
                    ImGui::Text("#%u", iss.objectId);
                }
                else
                {
                    ImGui::TextDisabled("-");
                }

                // 4. Title & Description
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(iss.title.c_str());
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s", iss.description.c_str());
                }

                // 5. Recommendation
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(iss.recommendation.c_str());
            }

            ImGui::EndTable();
        }

        ImGui::End();
    }
}
