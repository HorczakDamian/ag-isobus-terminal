#pragma once

#include "isobus/isobus/can_callbacks.hpp"
#include "isobus/isobus/can_constants.hpp"
#include "isobus/isobus/can_internal_control_function.hpp"
#include "isobus/isobus/isobus_virtual_terminal_server.hpp"
#include "isobus/isobus/isobus_virtual_terminal_objects.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace isobus_vt
{
    class CustomVTServer : public isobus::VirtualTerminalServer
    {
    public:
        explicit CustomVTServer(std::shared_ptr<isobus::InternalControlFunction> controlFunctionToUse);
        virtual ~CustomVTServer();

        // --- Pure virtual methods implementation ---
        bool get_is_enough_memory(std::uint32_t requestedMemory) const override;
        VTVersion get_version() const override;
        std::uint8_t get_number_of_navigation_soft_keys() const override;
        std::uint8_t get_soft_key_descriptor_x_pixel_width() const override;
        std::uint8_t get_soft_key_descriptor_y_pixel_height() const override;
        std::uint8_t get_number_of_possible_virtual_soft_keys_in_soft_key_mask() const override;
        std::uint8_t get_number_of_physical_soft_keys() const override;
        std::uint16_t get_data_mask_area_size_x_pixels() const override;
        std::uint16_t get_data_mask_area_size_y_pixels() const override;
        void suspend_working_set(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> workingSetWithError) override;
        SupportedWideCharsErrorCode get_supported_wide_chars(std::uint8_t codePlane,
                                                             std::uint16_t firstWideCharInInquiryRange,
                                                             std::uint16_t lastWideCharInInquiryRange,
                                                             std::uint8_t &numberOfRanges,
                                                             std::vector<std::uint8_t> &wideCharRangeArray) override;
        std::vector<std::array<std::uint8_t, 7>> get_versions(isobus::NAME clientNAME) override;
        std::vector<std::array<std::uint8_t, 32>> get_extended_versions(isobus::NAME clientNAME) override;
        std::vector<std::uint8_t> get_supported_objects() const override;
        std::vector<std::uint8_t> load_version(const std::vector<std::uint8_t> &versionLabel, isobus::NAME clientNAME) override;
        bool save_version(const std::vector<std::uint8_t> &objectPool, const std::vector<std::uint8_t> &versionLabel, isobus::NAME clientNAME) override;
        bool delete_version(const std::vector<std::uint8_t> &versionLabel, isobus::NAME clientNAME) override;
        bool delete_all_versions(std::uint8_t versionLabelLength, isobus::NAME clientNAME) override;
        bool delete_object_pool(isobus::NAME clientNAME) override;
        void handle_unexpected_working_set_shutdown(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> workingSet, UnexpectedShutdownReason reason) override;
        std::uint16_t abort_input(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> workingSet) override;

        // Overrides
        void object_pool_parse_finished(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> workingSet, bool success) override;
        void select_working_set(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> workingSet) override;

        // Public helpers for GUI and User interaction
        void run_update();
        std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> get_active_ws();
        std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> get_all_ws();
        std::uint16_t get_active_data_mask_id() const;
        std::uint16_t get_active_soft_key_mask_id() const;
        std::uint16_t get_working_set_active_mask(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws) const;
        bool is_alarm_active_on_ws(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws) const;

        bool activate_button(std::uint16_t objectId, std::uint16_t parentObjectId, std::uint8_t keyNumber, bool pressed);
        bool activate_soft_key(std::uint16_t objectId, std::uint16_t parentObjectId, std::uint8_t keyNumber, bool pressed);
        bool is_alarm_active() const;
        bool send_alarm_ack_command(bool pressed);
        bool change_numeric_value(std::uint16_t objectId, std::uint32_t value);
        bool change_string_value(std::uint16_t objectId, const std::string &value);
        void trigger_macro(std::shared_ptr<isobus::VTObject> object,
                           isobus::EventID macroEvent,
                           isobus::VirtualTerminalObjectType targetObjectType);
        bool switch_working_set(std::size_t index);

        std::recursive_mutex& get_server_mutex() const { return serverMutex; }
        void install_synchronized_callbacks();
        void remove_synchronized_callbacks();
        static void synchronized_process_rx_message(const isobus::CANMessage &message, void *parent);

        // Flag to inform renderer of updates
        bool consume_repaint_flag();

        // ISO 11783-7 Language & Localization command
        void apply_language_command_settings(const std::string &langCode,
                                             const std::string &countryCode,
                                             int decimalSymbol,
                                             int dateFormat,
                                             int unitSystem);

        // Runtime VT configuration — setters
        void set_vt_version(isobus::VirtualTerminalBase::VTVersion v) { vtVersion = v; }
        void set_data_mask_size(std::uint16_t px)   { dataMaskSizePx = px; }
        void set_sk_desc_width(std::uint8_t px)     { skDescWidth = px; }
        void set_sk_desc_height(std::uint8_t px)    { skDescHeight = px; }
        void set_sk_columns(std::uint8_t n)         { skColumns = n; }
        void set_sk_rows(std::uint8_t n)            { skRows = n; }
        void set_vt_number(std::uint8_t n)          { vtNumber = n; }

        // Runtime VT configuration — getters (for settings UI)
        isobus::VirtualTerminalBase::VTVersion get_configured_version() const { return vtVersion; }
        std::uint16_t get_configured_mask_size()  const { return dataMaskSizePx; }
        std::uint8_t  get_configured_sk_width()   const { return skDescWidth; }
        std::uint8_t  get_configured_sk_height()  const { return skDescHeight; }
        std::uint8_t  get_configured_sk_columns() const { return skColumns; }
        std::uint8_t  get_configured_sk_rows()    const { return skRows; }
        std::uint8_t  get_configured_vt_number()  const { return vtNumber; }

        // Object Pool Disk Cache functions
        void init_disk_cache();
        std::size_t get_cached_pool_count() const;
        std::size_t get_cached_pool_total_bytes() const;
        void clear_disk_cache();

    private:
        std::string get_cache_file_path(std::uint64_t clientNameRaw, const std::string &label) const;
        void scan_cache_directory();
        mutable std::recursive_mutex serverMutex;
        std::atomic<bool> repaintRequested{false};
        std::uint16_t currentOpenInputObjectID = isobus::NULL_OBJECT_ID;

        // Non-volatile version cache in memory (and disk)
        std::map<std::pair<std::uint64_t, std::string>, std::vector<std::uint8_t>> poolStorage;

        // Last active DataMask per Working Set (for universal ISO 11783-6 alarm dismissal/return)
        std::map<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>, std::uint16_t> lastActiveDataMask;

        // Runtime-configurable VT capabilities
        isobus::VirtualTerminalBase::VTVersion vtVersion{ isobus::VirtualTerminalBase::VTVersion::Version6 };
        std::uint16_t dataMaskSizePx{ 480 };
        std::uint8_t  skDescWidth{ 60 };
        std::uint8_t  skDescHeight{ 60 };
        std::uint8_t  skColumns{ 1 };
        std::uint8_t  skRows{ 6 };
        std::uint8_t  vtNumber{ 1 };
    };
}
