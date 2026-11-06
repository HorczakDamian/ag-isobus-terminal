#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>

namespace isobus_vt
{
    class AmazoneBridge
    {
    public:
        static AmazoneBridge& get_instance();

        AmazoneBridge();
        ~AmazoneBridge() = default;

        // Lifecycle & Configuration
        void initialize();
        void update();
        void set_enabled(bool enabled);
        bool is_enabled() const { return bridgeEnabled; }

        // Section & Control State
        void set_num_sections(std::uint8_t count);
        std::uint8_t get_num_sections() const { return numSections; }

        void set_auto_mode(bool autoMode);
        bool is_auto_mode() const { return autoControlMode; }

        void set_section_state(std::uint8_t index, bool state);
        bool get_section_state(std::uint8_t index) const;
        void toggle_section(std::uint8_t index);

        void set_all_sections(bool state);
        void invert_all_sections();

        void set_master_valve(bool open);
        bool get_master_valve() const { return masterValveOpen; }
        void toggle_master_valve();

        // Feed from AgOpenGPS UDP
        void on_aog_section_states(const std::vector<bool>& states);
        void on_aog_mode_received(bool autoMode);
        void on_aog_speed_received(float speedKph);

        // Hardware CAN Broadcast
        bool send_address_claim();
        bool send_section_frame();

        // Diagnostics & Metrics
        std::uint64_t get_total_tx_frames() const { return totalTxFrames; }
        std::uint32_t get_last_tx_time_ms() const { return lastTxTimestampMs; }
        bool is_aog_connected() const;
        float get_aog_speed() const { return lastGpsSpeedKph; }

        // Settings
        std::uint8_t get_byte7() const { return byte7Val; }
        void set_byte7(std::uint8_t val) { byte7Val = val; }
        std::uint32_t get_tx_rate_ms() const { return txIntervalMs; }
        void set_tx_rate_ms(std::uint32_t intervalMs) { txIntervalMs = intervalMs; }

        // UI
        void render_window(bool *p_open);
        void render_embedded();

    private:
        std::atomic<bool> bridgeEnabled{true};
        std::atomic<bool> autoControlMode{true};
        std::atomic<std::uint8_t> numSections{9};
        std::atomic<bool> masterValveOpen{false};
        std::atomic<bool> masterManualOverride{false};

        std::uint8_t byte7Val = 0x07; // 0x07 (standard test) or 0x09
        std::uint32_t txIntervalMs = 200; // 5 Hz default

        mutable std::recursive_mutex dataMutex;
        std::vector<bool> sections;

        // Statistics & Timers
        std::atomic<std::uint64_t> totalTxFrames{0};
        std::uint32_t lastTxTimestampMs = 0;
        std::uint32_t lastClaimTimestampMs = 0;
        std::uint32_t lastAogPacketTimestampMs = 0;
        float lastGpsSpeedKph = 0.0f;

        // CAN frame buffer cached
        std::uint8_t cachedByte2 = 0x00;
        std::uint8_t cachedByte3 = 0x00;
        std::string lastFrameHex;

        void compute_can_payload(std::uint8_t &b2, std::uint8_t &b3);
    };
}
