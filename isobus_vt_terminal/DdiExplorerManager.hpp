#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <deque>
#include <mutex>
#include <memory>

#include "isobus/isobus/isobus_standard_data_description_indices.hpp"

namespace isobus_vt
{
    struct DdiDefinition
    {
        std::uint16_t ddi = 0;
        std::string name;
        std::string unit;
        float resolution = 1.0f;
        std::int32_t offset = 0;
        std::string category; // Rate, Width, Section, Tank, Speed, Pressure, Misc
    };

    struct DdiLiveRecord
    {
        DdiDefinition def;
        std::int32_t rawValue = 0;
        float scaledValue = 0.0f;
        std::uint32_t lastUpdateTimestampMs = 0;
        std::uint32_t updateCount = 0;
        float minValue = 999999.0f;
        float maxValue = -999999.0f;
        float sumValues = 0.0f;
        std::deque<float> history; // Ring buffer for ImGui sparklines (last 60 samples)
        std::uint8_t sourceAddress = 0;
        std::uint16_t elementNumber = 0;
    };

    class DdiExplorerManager
    {
    public:
        static DdiExplorerManager& get_instance();

        bool is_enabled() const { return explorerEnabled; }
        void set_enabled(bool enabled) { explorerEnabled = enabled; }

        void update_ddi_value(std::uint16_t ddi, std::int32_t rawValue, std::uint8_t sa = 0, std::uint16_t element = 0);
        void record_process_data_from_can(std::uint32_t pgn, std::uint8_t sa, const std::uint8_t *data, std::uint32_t len);

        void render_ddi_explorer_window(bool *p_open);

        // Simulation helper
        void inject_sample_ddis();

    private:
        DdiExplorerManager();
        ~DdiExplorerManager() = default;

        void init_standard_dictionary();
        DdiDefinition lookup_definition(std::uint16_t ddi);

        mutable std::mutex dataMutex;
        std::map<std::uint16_t, DdiDefinition> dictionary;
        std::map<std::uint16_t, DdiLiveRecord> liveRecords;

        // UI state
        bool explorerEnabled = true;
        char searchFilter[64] = { 0 };
        int selectedCategoryIdx = 0;
        std::uint16_t selectedDdiForGraph = 0;
        bool autoScroll = true;
    };
}
