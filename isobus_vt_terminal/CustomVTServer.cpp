#include "CustomVTServer.hpp"
#include "TerminalSettings.hpp"
#include "isobus/isobus/can_network_manager.hpp"
#include "isobus/isobus/can_stack_logger.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace isobus_vt
{
    CustomVTServer::CustomVTServer(std::shared_ptr<isobus::InternalControlFunction> controlFunctionToUse) :
        VirtualTerminalServer(controlFunctionToUse)
    {
        get_on_repaint_event_dispatcher().add_listener([this](std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>) {
            repaintRequested = true;
        });

        get_on_change_active_mask_event_dispatcher().add_listener([this](std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws, std::uint16_t, std::uint16_t newMaskId) {
            std::lock_guard<std::recursive_mutex> lock(serverMutex);
            if (ws)
            {
                lastActiveDataMask[ws] = newMaskId;
                if (ws == activeWorkingSet)
                {
                    activeWorkingSetDataMaskObjectID = newMaskId;
                }
                auto maskObj = ws->get_object_by_id(newMaskId);
                if (maskObj)
                {
                    if (maskObj->get_object_type() == isobus::VirtualTerminalObjectType::DataMask)
                    {
                        if (ws == activeWorkingSet)
                        {
                            auto dm = std::dynamic_pointer_cast<isobus::DataMask>(maskObj);
                            if (dm && dm->get_soft_key_mask() != isobus::NULL_OBJECT_ID)
                                activeWorkingSetSoftkeyMaskObjectID = dm->get_soft_key_mask();
                            else
                                activeWorkingSetSoftkeyMaskObjectID = isobus::NULL_OBJECT_ID;
                        }
                        process_macro(maskObj, isobus::EventID::OnShow, isobus::VirtualTerminalObjectType::DataMask, ws);
                        process_macro(maskObj, isobus::EventID::OnChangeActiveMask, isobus::VirtualTerminalObjectType::DataMask, ws);
                    }
                    else if (maskObj->get_object_type() == isobus::VirtualTerminalObjectType::AlarmMask)
                    {
                        if (get_active_working_set() != ws)
                        {
                            select_working_set(ws);
                        }
                        auto am = std::dynamic_pointer_cast<isobus::AlarmMask>(maskObj);
                        if (am && am->get_soft_key_mask() != isobus::NULL_OBJECT_ID)
                            activeWorkingSetSoftkeyMaskObjectID = am->get_soft_key_mask();
                        else
                            activeWorkingSetSoftkeyMaskObjectID = isobus::NULL_OBJECT_ID;
                        process_macro(maskObj, isobus::EventID::OnShow, isobus::VirtualTerminalObjectType::AlarmMask, ws);
                        process_macro(maskObj, isobus::EventID::OnChangeActiveMask, isobus::VirtualTerminalObjectType::AlarmMask, ws);
                    }
                }
            }
            repaintRequested = true;
            LOG_INFO("[VT Server]: Active mask changed to %u", newMaskId);
        });

        get_on_change_active_softkey_mask_event_dispatcher().add_listener([this](std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws, std::uint16_t, std::uint16_t newSkMaskId) {
            std::lock_guard<std::recursive_mutex> lock(serverMutex);
            activeWorkingSetSoftkeyMaskObjectID = newSkMaskId;
            repaintRequested = true;
            LOG_INFO("[VT Server]: Active soft key mask changed to %u", newSkMaskId);
        });

        init_disk_cache();
        const auto &cfg = TerminalSettings::get_instance().get_config();
        apply_language_command_settings(cfg.langCode, cfg.countryCode, cfg.unitDecimal, cfg.unitDate, cfg.unitArea);
    }

    CustomVTServer::~CustomVTServer()
    {
        remove_synchronized_callbacks();
    }

    void CustomVTServer::install_synchronized_callbacks()
    {
        isobus::CANNetworkManager::CANNetwork.remove_global_parameter_group_number_callback(0x00E700, process_rx_message, this);
        isobus::CANNetworkManager::CANNetwork.remove_global_parameter_group_number_callback(0x00FE0D, process_rx_message, this);

        isobus::CANNetworkManager::CANNetwork.add_global_parameter_group_number_callback(0x00E700, synchronized_process_rx_message, this);
        isobus::CANNetworkManager::CANNetwork.add_global_parameter_group_number_callback(0x00FE0D, synchronized_process_rx_message, this);
    }

    void CustomVTServer::remove_synchronized_callbacks()
    {
        isobus::CANNetworkManager::CANNetwork.remove_global_parameter_group_number_callback(0x00E700, synchronized_process_rx_message, this);
        isobus::CANNetworkManager::CANNetwork.remove_global_parameter_group_number_callback(0x00FE0D, synchronized_process_rx_message, this);
    }

    void CustomVTServer::synchronized_process_rx_message(const isobus::CANMessage &message, void *parent)
    {
        auto server = static_cast<CustomVTServer*>(parent);
        if (!server) return;
        std::lock_guard<std::recursive_mutex> lock(server->serverMutex);
        process_rx_message(message, parent);
    }


    bool CustomVTServer::get_is_enough_memory(std::uint32_t) const
    {
        return true;
    }

    isobus::VirtualTerminalBase::VTVersion CustomVTServer::get_version() const
    {
        return vtVersion;
    }

    std::uint8_t CustomVTServer::get_number_of_navigation_soft_keys() const
    {
        return 0;
    }

    std::uint8_t CustomVTServer::get_soft_key_descriptor_x_pixel_width() const
    {
        return skDescWidth;
    }

    std::uint8_t CustomVTServer::get_soft_key_descriptor_y_pixel_height() const
    {
        return skDescHeight;
    }

    std::uint8_t CustomVTServer::get_number_of_possible_virtual_soft_keys_in_soft_key_mask() const
    {
        return static_cast<std::uint8_t>(std::min(255, static_cast<int>(skRows) * static_cast<int>(skColumns)));
    }

    std::uint8_t CustomVTServer::get_number_of_physical_soft_keys() const
    {
        return static_cast<std::uint8_t>(std::min(255, static_cast<int>(skRows) * static_cast<int>(skColumns)));
    }

    std::uint16_t CustomVTServer::get_data_mask_area_size_x_pixels() const
    {
        return dataMaskSizePx;
    }

    std::uint16_t CustomVTServer::get_data_mask_area_size_y_pixels() const
    {
        return dataMaskSizePx;
    }

    void CustomVTServer::suspend_working_set(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>)
    {
        LOG_INFO("[VT Server]: Suspending working set.");
    }

    isobus::VirtualTerminalBase::SupportedWideCharsErrorCode CustomVTServer::get_supported_wide_chars(std::uint8_t,
                                                                                                        std::uint16_t,
                                                                                                        std::uint16_t,
                                                                                                        std::uint8_t &numberOfRanges,
                                                                                                        std::vector<std::uint8_t> &)
    {
        numberOfRanges = 0;
        return static_cast<isobus::VirtualTerminalBase::SupportedWideCharsErrorCode>(0);
    }

    std::string CustomVTServer::get_cache_file_path(std::uint64_t clientNameRaw, const std::string &label) const
    {
        std::ostringstream ss;
        ss << "object_pool_cache/" << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << clientNameRaw;
        if (!label.empty())
        {
            ss << "_" << label;
        }
        ss << ".iop";
        return ss.str();
    }

    void CustomVTServer::init_disk_cache()
    {
        std::error_code ec;
        std::filesystem::create_directories("object_pool_cache", ec);
        scan_cache_directory();
    }

    void CustomVTServer::scan_cache_directory()
    {
        std::lock_guard<std::recursive_mutex> lock(serverMutex);
        poolStorage.clear();

        std::vector<std::string> searchDirs = {
            "object_pool_cache",
            "build_vt/Release/object_pool_cache",
            "../object_pool_cache"
        };

        std::error_code ec;
        std::size_t loadedCount = 0;

        for (const auto &cacheDir : searchDirs)
        {
            if (!std::filesystem::exists(cacheDir, ec)) continue;

            for (const auto &entry : std::filesystem::directory_iterator(cacheDir, ec))
            {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".iop")
            {
                std::string stem = entry.path().stem().string(); // e.g. "A000810000007777_v1.0" or "A000810000007777"
                auto underscorePos = stem.find('_');
                std::string nameHexStr = (underscorePos != std::string::npos) ? stem.substr(0, underscorePos) : stem;
                std::string label = (underscorePos != std::string::npos) ? stem.substr(underscorePos + 1) : "";

                std::uint64_t nameRaw = 0;
                try
                {
                    nameRaw = std::stoull(nameHexStr, nullptr, 16);
                }
                catch (...)
                {
                    continue;
                }

                std::ifstream f(entry.path(), std::ios::binary | std::ios::ate);
                if (f.is_open())
                {
                    std::streamsize sz = f.tellg();
                    f.seekg(0, std::ios::beg);
                    std::vector<std::uint8_t> buf(sz);
                    if (f.read(reinterpret_cast<char*>(buf.data()), sz))
                    {
                        poolStorage[std::make_pair(nameRaw, label)] = std::move(buf);
                        loadedCount++;
                    }
                }
            }
        }
        }
        LOG_INFO("[VT Server]: Initialized Object Pool Disk Cache: loaded %zu cached versions from disk", loadedCount);
    }

    std::size_t CustomVTServer::get_cached_pool_count() const
    {
        std::lock_guard<std::recursive_mutex> lock(serverMutex);
        return poolStorage.size();
    }

    std::size_t CustomVTServer::get_cached_pool_total_bytes() const
    {
        std::lock_guard<std::recursive_mutex> lock(serverMutex);
        std::size_t total = 0;
        for (const auto &p : poolStorage)
        {
            total += p.second.size();
        }
        return total;
    }

    void CustomVTServer::clear_disk_cache()
    {
        std::lock_guard<std::recursive_mutex> lock(serverMutex);
        poolStorage.clear();
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator("object_pool_cache", ec))
        {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".iop")
            {
                std::filesystem::remove(entry.path(), ec);
            }
        }
        LOG_INFO("[VT Server]: Cleared Object Pool Disk Cache.");
    }

    std::vector<std::uint8_t> CustomVTServer::get_supported_objects() const
    {
        using ObjType = isobus::VirtualTerminalObjectType;
        return {
            static_cast<std::uint8_t>(ObjType::WorkingSet),
            static_cast<std::uint8_t>(ObjType::DataMask),
            static_cast<std::uint8_t>(ObjType::AlarmMask),
            static_cast<std::uint8_t>(ObjType::Container),
            static_cast<std::uint8_t>(ObjType::SoftKeyMask),
            static_cast<std::uint8_t>(ObjType::Key),
            static_cast<std::uint8_t>(ObjType::Button),
            static_cast<std::uint8_t>(ObjType::InputBoolean),
            static_cast<std::uint8_t>(ObjType::InputString),
            static_cast<std::uint8_t>(ObjType::InputNumber),
            static_cast<std::uint8_t>(ObjType::InputList),
            static_cast<std::uint8_t>(ObjType::OutputString),
            static_cast<std::uint8_t>(ObjType::OutputNumber),
            static_cast<std::uint8_t>(ObjType::OutputLine),
            static_cast<std::uint8_t>(ObjType::OutputRectangle),
            static_cast<std::uint8_t>(ObjType::OutputEllipse),
            static_cast<std::uint8_t>(ObjType::OutputPolygon),
            static_cast<std::uint8_t>(ObjType::OutputMeter),
            static_cast<std::uint8_t>(ObjType::OutputLinearBarGraph),
            static_cast<std::uint8_t>(ObjType::OutputArchedBarGraph),
            static_cast<std::uint8_t>(ObjType::PictureGraphic),
            static_cast<std::uint8_t>(ObjType::NumberVariable),
            static_cast<std::uint8_t>(ObjType::StringVariable),
            static_cast<std::uint8_t>(ObjType::FontAttributes),
            static_cast<std::uint8_t>(ObjType::LineAttributes),
            static_cast<std::uint8_t>(ObjType::FillAttributes),
            static_cast<std::uint8_t>(ObjType::InputAttributes),
            static_cast<std::uint8_t>(ObjType::ObjectPointer),
            static_cast<std::uint8_t>(ObjType::Macro),
            static_cast<std::uint8_t>(ObjType::AuxiliaryFunctionType1),
            static_cast<std::uint8_t>(ObjType::AuxiliaryInputType1),
            static_cast<std::uint8_t>(ObjType::AuxiliaryFunctionType2),
            static_cast<std::uint8_t>(ObjType::AuxiliaryInputType2),
            static_cast<std::uint8_t>(ObjType::AuxiliaryControlDesignatorType2),
            static_cast<std::uint8_t>(ObjType::WindowMask),
            static_cast<std::uint8_t>(ObjType::KeyGroup),
            static_cast<std::uint8_t>(ObjType::GraphicsContext),
            static_cast<std::uint8_t>(ObjType::ColourMap)
        };
    }

    std::vector<std::array<std::uint8_t, 7>> CustomVTServer::get_versions(isobus::NAME clientNAME)
    {
        std::lock_guard<std::recursive_mutex> lock(serverMutex);
        std::vector<std::array<std::uint8_t, 7>> versions;
        std::uint64_t targetName = clientNAME.get_full_name();

        for (const auto &pair : poolStorage)
        {
            if (pair.first.first == targetName)
            {
                const std::string &label = pair.first.second;
                if (label.size() <= 7)
                {
                    std::array<std::uint8_t, 7> arr;
                    arr.fill(' ');
                    for (std::size_t i = 0; i < label.size(); ++i)
                    {
                        arr[i] = static_cast<std::uint8_t>(label[i]);
                    }
                    versions.push_back(arr);
                }
            }
        }
        LOG_INFO("[VT Server]: get_versions for 0x%llX returned %zu stored versions", targetName, versions.size());
        return versions;
    }

    std::vector<std::array<std::uint8_t, 32>> CustomVTServer::get_extended_versions(isobus::NAME clientNAME)
    {
        std::lock_guard<std::recursive_mutex> lock(serverMutex);
        std::vector<std::array<std::uint8_t, 32>> extVersions;
        std::uint64_t targetName = clientNAME.get_full_name();

        for (const auto &pair : poolStorage)
        {
            if (pair.first.first == targetName)
            {
                const std::string &label = pair.first.second;
                std::array<std::uint8_t, 32> arr;
                arr.fill(' ');
                for (std::size_t i = 0; i < std::min<std::size_t>(32, label.size()); ++i)
                {
                    arr[i] = static_cast<std::uint8_t>(label[i]);
                }
                extVersions.push_back(arr);
            }
        }
        LOG_INFO("[VT Server]: get_extended_versions for 0x%llX returned %zu stored versions", targetName, extVersions.size());
        return extVersions;
    }

    std::vector<std::uint8_t> CustomVTServer::load_version(const std::vector<std::uint8_t> &versionLabel, isobus::NAME clientNAME)
    {
        std::lock_guard<std::recursive_mutex> lock(serverMutex);
        std::string label(versionLabel.begin(), versionLabel.end());
        while (!label.empty() && (label.back() == ' ' || label.back() == '\0'))
            label.pop_back();

        // 1. Direct memory lookup
        auto key = std::make_pair(clientNAME.get_full_name(), label);
        auto it = poolStorage.find(key);
        if (it != poolStorage.end())
        {
            LOG_INFO("[VT Server]: Fast-Boot loaded cached pool for 0x%llX version '%s' (%zu bytes)!",
                     clientNAME.get_full_name(), label.c_str(), it->second.size());
            return it->second;
        }

        // 2. Fuzzy memory lookup for this client
        for (const auto &pair : poolStorage)
        {
            if (pair.first.first == clientNAME.get_full_name())
            {
                LOG_INFO("[VT Server]: Fast-Boot fuzzy matched cached pool for 0x%llX (stored '%s' vs requested '%s', %zu bytes)",
                         clientNAME.get_full_name(), pair.first.second.c_str(), label.c_str(), pair.second.size());
                return pair.second;
            }
        }

        // 3. Try reading exact path from disk
        std::string path = get_cache_file_path(clientNAME.get_full_name(), label);
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (file.is_open())
        {
            std::streamsize size = file.tellg();
            file.seekg(0, std::ios::beg);
            std::vector<std::uint8_t> buffer(size);
            if (file.read(reinterpret_cast<char*>(buffer.data()), size))
            {
                poolStorage[key] = buffer;
                LOG_INFO("[VT Server]: Read cached pool from disk file '%s' (%zu bytes)", path.c_str(), buffer.size());
                return buffer;
            }
        }

        // 4. Scan disk cache folder for any file belonging to this client NAME
        std::ostringstream ssHex;
        ssHex << std::hex << std::uppercase << std::setw(16) << std::setfill('0') << clientNAME.get_full_name();
        std::string hexClient = ssHex.str();
        std::error_code ec;
        if (std::filesystem::exists("object_pool_cache", ec))
        {
            for (const auto &entry : std::filesystem::directory_iterator("object_pool_cache", ec))
            {
                if (entry.is_regular_file(ec) && entry.path().extension() == ".iop")
                {
                    if (entry.path().filename().string().find(hexClient) != std::string::npos)
                    {
                        std::ifstream diskFile(entry.path(), std::ios::binary | std::ios::ate);
                        if (diskFile.is_open())
                        {
                            std::streamsize size = diskFile.tellg();
                            diskFile.seekg(0, std::ios::beg);
                            std::vector<std::uint8_t> buffer(size);
                            if (diskFile.read(reinterpret_cast<char*>(buffer.data()), size))
                            {
                                poolStorage[key] = buffer;
                                LOG_INFO("[VT Server]: Found and loaded cached pool from disk '%s' (%zu bytes)",
                                         entry.path().string().c_str(), buffer.size());
                                return buffer;
                            }
                        }
                    }
                }
            }
        }

        LOG_INFO("[VT Server]: No cached pool for 0x%llX version '%s'", clientNAME.get_full_name(), label.c_str());
        return {};
    }

    bool CustomVTServer::save_version(const std::vector<std::uint8_t> &objectPool, const std::vector<std::uint8_t> &versionLabel, isobus::NAME clientNAME)
    {
        std::lock_guard<std::recursive_mutex> lock(serverMutex);
        std::string label(versionLabel.begin(), versionLabel.end());
        while (!label.empty() && (label.back() == ' ' || label.back() == '\0'))
            label.pop_back();

        auto key = std::make_pair(clientNAME.get_full_name(), label);
        poolStorage[key] = objectPool;

        // Persist to disk
        std::string path = get_cache_file_path(clientNAME.get_full_name(), label);
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (file.is_open())
        {
            file.write(reinterpret_cast<const char*>(objectPool.data()), objectPool.size());
            file.close();
            LOG_INFO("[VT Server]: Persisted object pool cache to disk: %s (%zu bytes)", path.c_str(), objectPool.size());
        }

        return true;
    }

    bool CustomVTServer::delete_version(const std::vector<std::uint8_t> &versionLabel, isobus::NAME clientNAME)
    {
        std::lock_guard<std::recursive_mutex> lock(serverMutex);
        std::string label(versionLabel.begin(), versionLabel.end());
        while (!label.empty() && (label.back() == ' ' || label.back() == '\0'))
            label.pop_back();

        auto key = std::make_pair(clientNAME.get_full_name(), label);
        poolStorage.erase(key);

        std::string path = get_cache_file_path(clientNAME.get_full_name(), label);
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return true;
    }

    bool CustomVTServer::delete_all_versions(std::uint8_t, isobus::NAME clientNAME)
    {
        std::lock_guard<std::recursive_mutex> lock(serverMutex);
        for (auto it = poolStorage.begin(); it != poolStorage.end(); )
        {
            if (it->first.first == clientNAME.get_full_name())
            {
                std::string path = get_cache_file_path(it->first.first, it->first.second);
                std::error_code ec;
                std::filesystem::remove(path, ec);
                it = poolStorage.erase(it);
            }
            else
            {
                ++it;
            }
        }
        return true;
    }

    bool CustomVTServer::delete_object_pool(isobus::NAME clientNAME)
    {
        LOG_INFO("[VT Server]: Deleting volatile object pool for client 0x%llX", clientNAME.get_full_name());
        repaintRequested = true;
        return true;
    }

    void CustomVTServer::handle_unexpected_working_set_shutdown(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws, UnexpectedShutdownReason reason)
    {
        std::lock_guard<std::recursive_mutex> lock(serverMutex);
        LOG_WARNING("[VT Server]: Unexpected shutdown of working set, reason: %d", static_cast<int>(reason));
        if (ws)
        {
            lastActiveDataMask.erase(ws);
            if (activeWorkingSet == ws)
            {
                activeWorkingSet = nullptr;
                activeWorkingSetMasterAddress = isobus::NULL_CAN_ADDRESS;
                activeWorkingSetDataMaskObjectID = isobus::NULL_OBJECT_ID;
                activeWorkingSetSoftkeyMaskObjectID = isobus::NULL_OBJECT_ID;

                for (auto &other : managedWorkingSetList)
                {
                    if (other && other != ws && other->get_working_set_object())
                    {
                        select_working_set(other);
                        break;
                    }
                }
            }
        }
        repaintRequested = true;
    }

    std::uint16_t CustomVTServer::abort_input(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>)
    {
        auto obj = currentOpenInputObjectID;
        currentOpenInputObjectID = isobus::NULL_OBJECT_ID;
        return obj;
    }

    void CustomVTServer::object_pool_parse_finished(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> workingSet, bool success)
    {
        VirtualTerminalServer::object_pool_parse_finished(workingSet, success);
        LOG_INFO("[VT Server]: Object pool parse finished. Success = %d", success ? 1 : 0);
        if (success && workingSet && workingSet->get_working_set_object() != nullptr)
        {
            select_working_set(workingSet);
        }
        repaintRequested = true;
    }

    void CustomVTServer::select_working_set(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> workingSet)
    {
        std::lock_guard<std::recursive_mutex> lock(serverMutex);
        if (!workingSet)
        {
            activeWorkingSet = nullptr;
            activeWorkingSetMasterAddress = isobus::NULL_CAN_ADDRESS;
            activeWorkingSetDataMaskObjectID = isobus::NULL_OBJECT_ID;
            activeWorkingSetSoftkeyMaskObjectID = isobus::NULL_OBJECT_ID;
            repaintRequested = true;
            return;
        }

        auto wsObj = workingSet->get_working_set_object();
        if (!wsObj)
        {
            LOG_WARNING("[VT Server]: Cannot select working set - object pool not parsed yet.");
            return;
        }

        bool wasAlreadyActive = (activeWorkingSet == workingSet);
        activeWorkingSet = workingSet;
        if (workingSet->get_control_function())
        {
            activeWorkingSetMasterAddress = workingSet->get_control_function()->get_address();
        }
        else
        {
            activeWorkingSetMasterAddress = isobus::NULL_CAN_ADDRESS;
        }

        auto wsTyped = std::dynamic_pointer_cast<isobus::WorkingSet>(wsObj);

        // Find the root DataMask for this Working Set
        std::uint16_t rootMask = isobus::NULL_OBJECT_ID;
        if (wsTyped && wsTyped->get_active_mask() != isobus::NULL_OBJECT_ID)
        {
            rootMask = wsTyped->get_active_mask();
        }
        else if (wsTyped && wsTyped->get_number_children() > 0)
        {
            for (std::uint16_t c = 0; c < wsTyped->get_number_children(); ++c)
            {
                auto childId = wsTyped->get_child_id(c);
                if (childId != isobus::NULL_OBJECT_ID)
                {
                    auto child = workingSet->get_object_by_id(childId);
                    if (child && child->get_object_type() == isobus::VirtualTerminalObjectType::DataMask)
                    {
                        rootMask = childId;
                        break;
                    }
                }
            }
        }

        if (rootMask == isobus::NULL_OBJECT_ID)
        {
            // Fallback: search object tree for any DataMask
            const auto &tree = workingSet->get_object_tree();
            for (const auto &[id, obj] : tree)
            {
                if (obj && obj->get_object_type() == isobus::VirtualTerminalObjectType::DataMask)
                {
                    rootMask = id;
                    break;
                }
            }
        }

        std::uint16_t targetMask = isobus::NULL_OBJECT_ID;
        // If the operator clicks the working set that is ALREADY ACTIVE, return to root/home DataMask!
        // Otherwise, restore the last active mask recorded for this working set (or fallback to rootMask).
        if (wasAlreadyActive && rootMask != isobus::NULL_OBJECT_ID)
        {
            targetMask = rootMask;
        }
        else
        {
            auto it = lastActiveDataMask.find(workingSet);
            if (it != lastActiveDataMask.end() && it->second != isobus::NULL_OBJECT_ID)
                targetMask = it->second;
            else
                targetMask = rootMask;
        }

        if (targetMask != isobus::NULL_OBJECT_ID)
        {
            activeWorkingSetDataMaskObjectID = targetMask;
            lastActiveDataMask[workingSet] = targetMask;
            if (wsTyped)
            {
                wsTyped->set_active_mask(targetMask);
            }

            auto maskObj = workingSet->get_object_by_id(targetMask);
            if (maskObj)
            {
                if (maskObj->get_object_type() == isobus::VirtualTerminalObjectType::DataMask)
                {
                    auto dm = std::dynamic_pointer_cast<isobus::DataMask>(maskObj);
                    if (dm && dm->get_soft_key_mask() != isobus::NULL_OBJECT_ID)
                        activeWorkingSetSoftkeyMaskObjectID = dm->get_soft_key_mask();
                    else
                        activeWorkingSetSoftkeyMaskObjectID = isobus::NULL_OBJECT_ID;
                }
                else if (maskObj->get_object_type() == isobus::VirtualTerminalObjectType::AlarmMask)
                {
                    auto am = std::dynamic_pointer_cast<isobus::AlarmMask>(maskObj);
                    if (am && am->get_soft_key_mask() != isobus::NULL_OBJECT_ID)
                        activeWorkingSetSoftkeyMaskObjectID = am->get_soft_key_mask();
                    else
                        activeWorkingSetSoftkeyMaskObjectID = isobus::NULL_OBJECT_ID;
                }

                process_macro(maskObj, isobus::EventID::OnShow, maskObj->get_object_type(), workingSet);
            }
        }

        LOG_INFO("[VT Server]: Selected working set as active. DataMask=%u, SoftKeyMask=%u",
                 activeWorkingSetDataMaskObjectID, activeWorkingSetSoftkeyMaskObjectID);
        repaintRequested = true;
    }

    void CustomVTServer::run_update()
    {
        std::lock_guard<std::recursive_mutex> lock(serverMutex);
        update();
    }

    std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> CustomVTServer::get_active_ws()
    {
        std::lock_guard<std::recursive_mutex> lock(serverMutex);
        return get_active_working_set();
    }

    std::vector<std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet>> CustomVTServer::get_all_ws()
    {
        std::lock_guard<std::recursive_mutex> lock(serverMutex);
        return managedWorkingSetList;
    }

    std::uint16_t CustomVTServer::get_active_data_mask_id() const
    {
        std::lock_guard<std::recursive_mutex> lock(serverMutex);
        if (activeWorkingSet && activeWorkingSet->get_working_set_object())
        {
            auto wsObj = std::dynamic_pointer_cast<isobus::WorkingSet>(activeWorkingSet->get_working_set_object());
            if (wsObj && wsObj->get_active_mask() != isobus::NULL_OBJECT_ID)
            {
                return wsObj->get_active_mask();
            }
        }
        return activeWorkingSetDataMaskObjectID;
    }

    std::uint16_t CustomVTServer::get_active_soft_key_mask_id() const
    {
        std::lock_guard<std::recursive_mutex> lock(serverMutex);
        if (activeWorkingSetSoftkeyMaskObjectID != isobus::NULL_OBJECT_ID)
        {
            return activeWorkingSetSoftkeyMaskObjectID;
        }
        if (activeWorkingSet)
        {
            std::uint16_t maskId = activeWorkingSetDataMaskObjectID;
            if (activeWorkingSet->get_working_set_object())
            {
                auto wsObj = std::dynamic_pointer_cast<isobus::WorkingSet>(activeWorkingSet->get_working_set_object());
                if (wsObj && wsObj->get_active_mask() != isobus::NULL_OBJECT_ID)
                {
                    maskId = wsObj->get_active_mask();
                }
            }
            auto maskObj = activeWorkingSet->get_object_by_id(maskId);
            if (maskObj)
            {
                auto dm = std::dynamic_pointer_cast<isobus::DataMask>(maskObj);
                if (dm && dm->get_soft_key_mask() != isobus::NULL_OBJECT_ID)
                    return dm->get_soft_key_mask();
                auto am = std::dynamic_pointer_cast<isobus::AlarmMask>(maskObj);
                if (am && am->get_soft_key_mask() != isobus::NULL_OBJECT_ID)
                    return am->get_soft_key_mask();
            }
        }
        return isobus::NULL_OBJECT_ID;
    }

    bool CustomVTServer::activate_button(std::uint16_t objectId, std::uint16_t parentObjectId, std::uint8_t keyNumber, bool pressed)
    {
        auto ws = get_active_ws();
        if (!ws || !ws->get_control_function())
            return false;

        auto code = pressed ? isobus::VirtualTerminalBase::KeyActivationCode::ButtonPressedOrLatched : isobus::VirtualTerminalBase::KeyActivationCode::ButtonUnlatchedOrReleased;
        return send_button_activation_message(code, objectId, parentObjectId, keyNumber, ws->get_control_function());
    }

    bool CustomVTServer::activate_soft_key(std::uint16_t objectId, std::uint16_t parentObjectId, std::uint8_t keyNumber, bool pressed)
    {
        auto ws = get_active_ws();
        if (!ws || !ws->get_control_function())
            return false;

        auto code = pressed ? isobus::VirtualTerminalBase::KeyActivationCode::ButtonPressedOrLatched : isobus::VirtualTerminalBase::KeyActivationCode::ButtonUnlatchedOrReleased;
        return send_soft_key_activation_message(code, objectId, parentObjectId, keyNumber, ws->get_control_function());
    }

    std::uint16_t CustomVTServer::get_working_set_active_mask(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws) const
    {
        if (!ws) return isobus::NULL_OBJECT_ID;
        std::lock_guard<std::recursive_mutex> lock(serverMutex);
        if (ws->get_working_set_object())
        {
            auto wsObj = std::dynamic_pointer_cast<isobus::WorkingSet>(ws->get_working_set_object());
            if (wsObj)
            {
                if (wsObj->get_active_mask() != isobus::NULL_OBJECT_ID)
                    return wsObj->get_active_mask();
                if (wsObj->get_number_children() > 0)
                {
                    for (std::uint16_t c = 0; c < wsObj->get_number_children(); ++c)
                    {
                        auto child = ws->get_object_by_id(wsObj->get_child_id(c));
                        if (child && (child->get_object_type() == isobus::VirtualTerminalObjectType::DataMask ||
                                      child->get_object_type() == isobus::VirtualTerminalObjectType::AlarmMask))
                        {
                            return child->get_id();
                        }
                    }
                }
            }
        }
        // Fallback: search object tree for any DataMask
        const auto &tree = ws->get_object_tree();
        for (const auto &[id, obj] : tree)
        {
            if (obj && obj->get_object_type() == isobus::VirtualTerminalObjectType::DataMask)
                return id;
        }
        return isobus::NULL_OBJECT_ID;
    }

    bool CustomVTServer::is_alarm_active_on_ws(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws) const
    {
        if (!ws) return false;
        auto maskId = get_working_set_active_mask(ws);
        if (maskId == isobus::NULL_OBJECT_ID) return false;
        auto maskObj = ws->get_object_by_id(maskId);
        return (maskObj && maskObj->get_object_type() == isobus::VirtualTerminalObjectType::AlarmMask);
    }

    bool CustomVTServer::is_alarm_active() const
    {
        auto ws = const_cast<CustomVTServer*>(this)->get_active_ws();
        if (!ws) return false;

        auto maskId = get_active_data_mask_id();
        if (maskId == isobus::NULL_OBJECT_ID) return false;

        auto activeMask = ws->get_object_by_id(maskId);
        return (activeMask && activeMask->get_object_type() == isobus::VirtualTerminalObjectType::AlarmMask);
    }

    bool CustomVTServer::send_alarm_ack_command(bool pressed)
    {
        auto ws = get_active_ws();
        if (!ws || !ws->get_control_function())
            return false;

        auto code = pressed ? isobus::VirtualTerminalBase::KeyActivationCode::ButtonPressedOrLatched
                            : isobus::VirtualTerminalBase::KeyActivationCode::ButtonUnlatchedOrReleased;

        auto maskId = get_active_data_mask_id();
        if (maskId == isobus::NULL_OBJECT_ID) return false;

        // Standard ISO 11783-6 alarm acknowledge soft key activation:
        // Key Number: 0 (Physical / Virtual ESC / Alarm Acknowledge Key)
        // Object ID: 0xFFFF (isobus::NULL_OBJECT_ID)
        // Parent Data Mask ID: maskId (the currently displayed AlarmMask)
        // Destination: Working Set Control Function
        bool sent = send_soft_key_activation_message(code, isobus::NULL_OBJECT_ID, maskId, 0, ws->get_control_function());

        // Upon button release, dismiss alarm mask and restore previous DataMask per ISO 11783-6 Table B.2
        if (!pressed)
        {
            auto activeMask = ws->get_object_by_id(maskId);
            if (activeMask && activeMask->get_object_type() == isobus::VirtualTerminalObjectType::AlarmMask)
            {
                std::uint16_t prevMask = isobus::NULL_OBJECT_ID;
                auto it = lastActiveDataMask.find(ws);
                if (it != lastActiveDataMask.end() && it->second != isobus::NULL_OBJECT_ID)
                {
                    prevMask = it->second;
                }
                else
                {
                    // If no previous DataMask recorded, find first DataMask in implement object pool
                    for (const auto &[id, obj] : ws->get_object_tree())
                    {
                        if (obj && obj->get_object_type() == isobus::VirtualTerminalObjectType::DataMask)
                        {
                            prevMask = id;
                            break;
                        }
                    }
                }

                if (prevMask != isobus::NULL_OBJECT_ID && prevMask != maskId)
                {
                    auto wsObj = std::dynamic_pointer_cast<isobus::WorkingSet>(ws->get_working_set_object());
                    if (wsObj)
                    {
                        wsObj->set_active_mask(prevMask);
                        activeWorkingSetDataMaskObjectID = prevMask;
                        auto prevObj = ws->get_object_by_id(prevMask);
                        if (prevObj)
                        {
                            auto dm = std::dynamic_pointer_cast<isobus::DataMask>(prevObj);
                            if (dm && dm->get_soft_key_mask() != isobus::NULL_OBJECT_ID)
                                activeWorkingSetSoftkeyMaskObjectID = dm->get_soft_key_mask();
                            else
                                activeWorkingSetSoftkeyMaskObjectID = isobus::NULL_OBJECT_ID;
                            process_macro(prevObj, isobus::EventID::OnShow, isobus::VirtualTerminalObjectType::DataMask, ws);
                            process_macro(prevObj, isobus::EventID::OnChangeActiveMask, isobus::VirtualTerminalObjectType::DataMask, ws);
                        }
                        repaintRequested = true;
                    }
                }
            }
        }
        return sent;
    }

    bool CustomVTServer::change_numeric_value(std::uint16_t objectId, std::uint32_t value)
    {
        auto ws = get_active_ws();
        if (!ws || !ws->get_control_function())
            return false;

        return send_change_numeric_value_message(objectId, value, ws->get_control_function());
    }

    bool CustomVTServer::change_string_value(std::uint16_t objectId, const std::string &value)
    {
        auto ws = get_active_ws();
        if (!ws || !ws->get_control_function())
            return false;

        return send_change_string_value_message(objectId, value, ws->get_control_function());
    }

    void CustomVTServer::trigger_macro(std::shared_ptr<isobus::VTObject> object,
                                       isobus::EventID macroEvent,
                                       isobus::VirtualTerminalObjectType targetObjectType)
    {
        auto ws = get_active_ws();
        if (ws && object)
        {
            process_macro(object, macroEvent, targetObjectType, ws);
        }
    }

    bool CustomVTServer::switch_working_set(std::size_t index)
    {
        std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> targetWs = nullptr;
        {
            std::lock_guard<std::recursive_mutex> lock(serverMutex);
            if (index < managedWorkingSetList.size())
            {
                targetWs = managedWorkingSetList[index];
            }
        }
        if (targetWs)
        {
            select_working_set(targetWs);
            return true;
        }
        return false;
    }

    bool CustomVTServer::consume_repaint_flag()
    {
        return repaintRequested.exchange(false);
    }

    void CustomVTServer::apply_language_command_settings(const std::string &langCode,
                                                         const std::string &countryCode,
                                                         int decimalSymbol,
                                                         int dateFormat,
                                                         int unitSystem)
    {
        std::lock_guard<std::recursive_mutex> lock(serverMutex);
        auto &lang = get_language_command_interface();

        lang.set_language_code(langCode.empty() ? "pl" : langCode);
        lang.set_country_code(countryCode.empty() ? "PL" : countryCode);
        lang.set_commanded_decimal_symbol(decimalSymbol == 0 ? isobus::LanguageCommandInterface::DecimalSymbols::Comma
                                                             : isobus::LanguageCommandInterface::DecimalSymbols::Point);
        lang.set_commanded_time_format(isobus::LanguageCommandInterface::TimeFormats::TwentyFourHour);

        auto df = isobus::LanguageCommandInterface::DateFormats::ddmmyyyy;
        switch (dateFormat)
        {
            case 0: df = isobus::LanguageCommandInterface::DateFormats::ddmmyyyy; break;
            case 1: df = isobus::LanguageCommandInterface::DateFormats::ddyyyymm; break;
            case 2: df = isobus::LanguageCommandInterface::DateFormats::mmddyyyy; break;
            case 3: df = isobus::LanguageCommandInterface::DateFormats::yyyymmdd; break;
            case 4: df = isobus::LanguageCommandInterface::DateFormats::yyyyddmm; break;
            default: df = isobus::LanguageCommandInterface::DateFormats::ddmmyyyy; break;
        }
        lang.set_commanded_date_format(df);

        bool isImperial = (unitSystem == 1);
        lang.set_commanded_distance_units(isImperial ? isobus::LanguageCommandInterface::DistanceUnits::ImperialUS
                                                     : isobus::LanguageCommandInterface::DistanceUnits::Metric);
        lang.set_commanded_area_units(isImperial ? isobus::LanguageCommandInterface::AreaUnits::ImperialUS
                                                 : isobus::LanguageCommandInterface::AreaUnits::Metric);
        lang.set_commanded_volume_units(isImperial ? isobus::LanguageCommandInterface::VolumeUnits::Imperial
                                                   : isobus::LanguageCommandInterface::VolumeUnits::Metric);
        lang.set_commanded_mass_units(isImperial ? isobus::LanguageCommandInterface::MassUnits::Imperial
                                                 : isobus::LanguageCommandInterface::MassUnits::Metric);
        lang.set_commanded_pressure_units(isImperial ? isobus::LanguageCommandInterface::PressureUnits::ImperialUS
                                                     : isobus::LanguageCommandInterface::PressureUnits::Metric);
        lang.set_commanded_temperature_units(isImperial ? isobus::LanguageCommandInterface::TemperatureUnits::ImperialUS
                                                        : isobus::LanguageCommandInterface::TemperatureUnits::Metric);
        lang.set_commanded_force_units(isImperial ? isobus::LanguageCommandInterface::ForceUnits::ImperialUS
                                                  : isobus::LanguageCommandInterface::ForceUnits::Metric);
        lang.set_commanded_generic_units(isImperial ? isobus::LanguageCommandInterface::UnitSystem::Imperial
                                                    : isobus::LanguageCommandInterface::UnitSystem::Metric);

        lang.send_language_command();
        LOG_INFO("[VT Server]: Sent ISO 11783-7 Language Command (Lang='%s', Country='%s', Dec=%d, Units=%s)",
                 lang.get_language_code().c_str(), lang.get_country_code().c_str(), decimalSymbol,
                 isImperial ? "Imperial" : "Metric");
    }
}
