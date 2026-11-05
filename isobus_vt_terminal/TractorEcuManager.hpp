#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <mutex>
#include <atomic>
#include <functional>
#include <memory>

#include "isobus/hardware_integration/PCANBasic.h"

namespace isobus_vt
{
    enum class TractorBrandProfile : std::uint8_t
    {
        StandardIsoJ1939 = 0,
        JohnDeere,
        FendtVario,
        CaseNewHolland,
        Claas,
        DeutzFahr,
        Custom
    };

    enum class TractorCanSource : std::uint8_t
    {
        SecondPcanUsb = 0,   // Dedykowany drugi adapter PCAN-USB (np. PCAN_USBBUS2)
        SharedWithCan1,      // Pobieranie ramek z pierwszego magistrali (gdy ciągnik ma 1 gniazdo ISOBUS)
        SimulationMode       // Tryb symulacyjny na stole
    };

    struct TractorModelDef
    {
        std::string displayName;
        std::string fileName;
        std::string brandLogoFile;
        TractorBrandProfile profile;
    };

    struct PgnFieldConfig
    {
        std::string name;
        std::uint32_t pgn = 0;
        std::uint8_t startByte = 0;   // 0..7
        std::uint8_t lengthBytes = 1; // 1, 2, 4
        float scale = 1.0f;
        float offset = 0.0f;
        float minVal = 0.0f;
        float maxVal = 100.0f;
        std::string unit;
        bool isLittleEndian = true;
    };

    struct SniffedCanFrame
    {
        std::uint32_t pgn = 0;
        std::uint8_t sa = 0;
        std::uint8_t dlc = 0;
        std::uint8_t data[8] = { 0 };
        std::uint8_t prevData[8] = { 0 };
        std::uint32_t lastTimeMs = 0;
        std::uint32_t count = 0;
        bool byteChanged[8] = { false };
    };

    struct TractorLiveTelemetry
    {
        // TUZ (Tractor Hitch)
        float rearHitchPositionPct = 0.0f; // 0..100% (0 = pełne opuszczenie w ziemię, 100 = podniesiony)
        float frontHitchPositionPct = 0.0f; // 0..100%
        bool isRearHitchLowered = false;
        bool isFrontHitchLowered = false;
        std::uint8_t hitchStatusBitfield = 0;

        // WOM (PTO)
        float rearPtoRpm = 0.0f;
        float frontPtoRpm = 0.0f;
        bool isRearPtoActive = false;
        bool isFrontPtoActive = false;

        // Silnik (Engine)
        float engineRpm = 0.0f;
        float engineLoadPct = 0.0f;
        float coolantTempC = 0.0f;
        float oilTempC = 0.0f;
        float boostPressureKpa = 0.0f;
        float engineHours = 0.0f;

        // Paliwo (Fuel)
        float fuelRateLitersPerHour = 0.0f;
        float avgFuelEconomyLitersPerHa = 0.0f;
        float fuelLevelPct = 0.0f;

        // Prędkości & Poślizg (Speed & Slip)
        float groundSpeedKph = 0.0f;
        float wheelSpeedKph = 0.0f;
        float wheelSlipPct = 0.0f;

        // Zasilanie
        float batteryVoltage = 13.8f;

        // Status automatycznej pracy (zgodny z TUZ / WOM)
        bool isWorkConditionMet = false;

        std::uint32_t lastMessageTimeMs = 0;
        std::uint64_t totalFramesReceived = 0;
    };

    class TractorEcuManager
    {
    public:
        static TractorEcuManager& get_instance();

        // Direct3D 11 initialization for texture rendering
        void set_d3d_device(ID3D11Device* device);

        // Cykl życia i komunikacja CAN
        bool start();
        void stop();
        void update();

        void set_source(TractorCanSource src);
        TractorCanSource get_source() const { return currentSource; }

        void set_pcan_channel(TPCANHandle channel);
        TPCANHandle get_pcan_channel() const { return pcanChannel; }

        void set_pcan_baudrate(TPCANBaudrate baud);
        TPCANBaudrate get_pcan_baudrate() const { return pcanBaud; }

        bool is_connected() const { return isConnected; }
        std::string get_status_string() const;

        // Ręczne wstrzykiwanie ramek (dla trybu SharedWithCan1)
        void process_can_frame(std::uint32_t id, const std::uint8_t *data, std::uint8_t len);

        // Telemetria na żywo
        TractorLiveTelemetry get_telemetry();
        bool is_work_active() const;

        // Wybór modelu i marki ciągnika
        const std::vector<TractorModelDef>& get_available_models() const { return tractorModels; }
        int get_selected_model_index() const { return selectedModelIndex; }
        void set_selected_model_index(int index);

        // Profile marek
        void load_brand_profile(TractorBrandProfile profile);
        TractorBrandProfile get_current_profile() const { return currentProfile; }

        // Konfiguracja PGN
        std::vector<PgnFieldConfig>& get_field_configs() { return fieldConfigs; }
        void set_hitch_work_threshold_pct(float thresholdPct) { hitchLoweredThresholdPct = thresholdPct; }
        float get_hitch_work_threshold_pct() const { return hitchLoweredThresholdPct; }

        void set_auto_work_trigger_enabled(bool enabled) { autoWorkTriggerEnabled = enabled; }
        bool get_auto_work_trigger_enabled() const { return autoWorkTriggerEnabled; }

        // Interfejs ImGui
        void render_tractor_dashboard(bool *p_open);
        void render_pgn_config_dialog(bool *p_open);

        // Sniffer
        std::vector<SniffedCanFrame> get_sniffed_frames();
        void clear_sniffer();

    private:
        TractorEcuManager();
        ~TractorEcuManager();

        void init_models_list();
        void load_model_texture(int modelIdx);
        void init_default_configs(TractorBrandProfile profile);
        void decode_frame(std::uint32_t pgn, std::uint8_t sa, const std::uint8_t *data, std::uint8_t len);
        void update_simulation();

        ID3D11ShaderResourceView* load_texture_from_file(const std::string &path, int &outW, int &outH);

        std::mutex dataMutex;
        ID3D11Device* d3dDevice = nullptr;
        bool gdiplusInitialized = false;
        ULONG_PTR gdiplusToken = 0;

        TractorCanSource currentSource = TractorCanSource::SecondPcanUsb;
        TPCANHandle pcanChannel = PCAN_USBBUS2; // Domyślnie drugi PCAN
        TPCANBaudrate pcanBaud = PCAN_BAUD_250K; // Domyślnie 250k standard J1939/ISOBUS
        std::atomic<bool> isConnected{ false };
        std::string statusText;

        // Modele ciągników
        std::vector<TractorModelDef> tractorModels;
        int selectedModelIndex = 0; // Domyślnie John Deere
        ID3D11ShaderResourceView* currentTractorTexture = nullptr;
        ID3D11ShaderResourceView* currentBrandTexture = nullptr;
        int textureWidth = 0;
        int textureHeight = 0;
        int logoWidth = 0;
        int logoHeight = 0;

        TractorBrandProfile currentProfile = TractorBrandProfile::JohnDeere;
        std::vector<PgnFieldConfig> fieldConfigs;
        float hitchLoweredThresholdPct = 25.0f; // Poniżej 25% podnośnika = stan roboczy (ziemia)
        bool autoWorkTriggerEnabled = true;

        TractorLiveTelemetry telemetry;
        std::map<std::uint32_t, SniffedCanFrame> sniffedFrames;

        // Zmienne symulacji
        float simHitch = 10.0f;
        float simPto = 540.0f;
        float simEngineRpm = 1800.0f;
        float simFuelRate = 18.5f;
        float simCoolant = 85.0f;
        float simFuelLevel = 76.0f;
    };
}
