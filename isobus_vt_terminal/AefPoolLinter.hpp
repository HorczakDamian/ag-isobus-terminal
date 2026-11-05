#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <set>

#include "isobus/isobus/isobus_virtual_terminal_objects.hpp"

namespace isobus_vt
{
    class CustomVTServer;

    enum class LinterSeverity : std::uint8_t
    {
        Info = 0,
        Warning = 1,
        Error = 2
    };

    struct LinterIssue
    {
        std::string ruleId; // e.g. AEF-RULE-01, ISO-11783-6-02
        LinterSeverity severity;
        std::uint16_t objectId = 0xFFFF;
        std::string title;
        std::string description;
        std::string recommendation;
    };

    struct LinterReport
    {
        std::uint32_t totalObjects = 0;
        std::uint32_t criticalErrorsCount = 0;
        std::uint32_t warningsCount = 0;
        std::uint32_t infosCount = 0;
        float aefScorePct = 100.0f;
        std::string workingSetLabel;
        std::vector<LinterIssue> issues;
        std::uint32_t scanTimestampMs = 0;
    };

    class AefPoolLinter
    {
    public:
        static AefPoolLinter& get_instance();

        void set_vt_server(std::shared_ptr<CustomVTServer> server) { vtServer = server; }

        LinterReport run_audit();

        void render_linter_window(bool *p_open);
        bool export_report_to_html(const std::string &filePath);

    private:
        AefPoolLinter() = default;
        ~AefPoolLinter() = default;

        bool check_cycles_dfs(std::uint16_t currentId,
                              const std::map<std::uint16_t, std::shared_ptr<isobus::VTObject>> &tree,
                              std::set<std::uint16_t> &visited,
                              std::set<std::uint16_t> &recursionStack,
                              std::vector<std::uint16_t> &cyclePath);

        std::shared_ptr<CustomVTServer> vtServer = nullptr;
        LinterReport currentReport;
        bool hasRunOnce = false;
        int filterSeverity = -1; // -1: All, 0: Info, 1: Warning, 2: Error
    };
}
