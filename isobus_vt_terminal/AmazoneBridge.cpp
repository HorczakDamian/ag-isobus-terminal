#include "AmazoneBridge.hpp"
#include "CanManager.hpp"
#include "TerminalSettings.hpp"
#include "isobus/isobus/can_hardware_abstraction.hpp"
#include "isobus/isobus/can_message_frame.hpp"
#include "imgui.h"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <algorithm>

namespace isobus_vt
{
    static std::uint32_t get_current_time_ms()
    {
        return static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    AmazoneBridge& AmazoneBridge::get_instance()
    {
        static AmazoneBridge instance;
        return instance;
    }

    AmazoneBridge::AmazoneBridge()
    {
        sections.resize(13, false);
        initialize();
    }

    void AmazoneBridge::initialize()
    {
        std::lock_guard<std::recursive_mutex> lock(dataMutex);
        numSections = 9;
        sections.assign(13, false);
        masterValveOpen = false;
        totalTxFrames = 0;
        lastTxTimestampMs = 0;
        lastClaimTimestampMs = 0;
        lastAogPacketTimestampMs = 0;
        lastGpsSpeedKph = 0.0f;
    }

    void AmazoneBridge::set_enabled(bool enabled)
    {
        bridgeEnabled = enabled;
        TerminalSettings::get_instance().get_config().amazoneBridgeEnabled = enabled;
        if (enabled)
        {
            send_address_claim();
        }
    }

    void AmazoneBridge::set_num_sections(std::uint8_t count)
    {
        std::lock_guard<std::recursive_mutex> lock(dataMutex);
        numSections = std::clamp<std::uint8_t>(count, 1, 13);
    }

    void AmazoneBridge::set_auto_mode(bool autoMode)
    {
        autoControlMode = autoMode;
    }

    void AmazoneBridge::set_section_state(std::uint8_t index, bool state)
    {
        {
            std::lock_guard<std::recursive_mutex> lock(dataMutex);
            if (index < sections.size())
            {
                sections[index] = state;
                
                bool anyOn = false;
                for (std::size_t i = 0; i < numSections; ++i)
                {
                    if (sections[i]) { anyOn = true; break; }
                }
                masterValveOpen = anyOn;
            }
        }
        send_section_frame();
    }

    bool AmazoneBridge::get_section_state(std::uint8_t index) const
    {
        std::lock_guard<std::recursive_mutex> lock(dataMutex);
        if (index < sections.size()) return sections[index];
        return false;
    }

    void AmazoneBridge::toggle_section(std::uint8_t index)
    {
        {
            std::lock_guard<std::recursive_mutex> lock(dataMutex);
            if (index < sections.size())
            {
                sections[index] = !sections[index];
                bool anyOn = false;
                for (std::size_t i = 0; i < numSections; ++i)
                {
                    if (sections[i]) { anyOn = true; break; }
                }
                masterValveOpen = anyOn;
            }
        }
        send_section_frame();
    }

    void AmazoneBridge::set_all_sections(bool state)
    {
        {
            std::lock_guard<std::recursive_mutex> lock(dataMutex);
            for (std::size_t i = 0; i < numSections; ++i)
            {
                sections[i] = state;
            }
            masterValveOpen = state;
        }
        send_section_frame();
    }

    void AmazoneBridge::invert_all_sections()
    {
        {
            std::lock_guard<std::recursive_mutex> lock(dataMutex);
            bool anyOn = false;
            for (std::size_t i = 0; i < numSections; ++i)
            {
                sections[i] = !sections[i];
                if (sections[i]) anyOn = true;
            }
            masterValveOpen = anyOn;
        }
        send_section_frame();
    }

    void AmazoneBridge::set_master_valve(bool open)
    {
        masterValveOpen = open;
        send_section_frame();
    }

    void AmazoneBridge::toggle_master_valve()
    {
        masterValveOpen = !masterValveOpen;
        send_section_frame();
    }

    void AmazoneBridge::on_aog_section_states(const std::vector<bool>& states)
    {
        lastAogPacketTimestampMs = get_current_time_ms();
        if (!bridgeEnabled || !autoControlMode) return;

        bool anyOn = false;
        std::string secStr = "[AOG UDP] Sekcje z AgOpenGPS: ";
        {
            std::lock_guard<std::recursive_mutex> lock(dataMutex);
            for (std::size_t i = 0; i < numSections && i < states.size(); ++i)
            {
                sections[i] = states[i];
                if (sections[i]) anyOn = true;
                secStr += "S" + std::to_string(i + 1) + (states[i] ? "=1 " : "=0 ");
            }
            masterValveOpen = anyOn;
        }
        if (CanManager::get_active_instance())
        {
            secStr += "| Master: " + std::string(anyOn ? "OTWARTY" : "ZAMKNIETY");
            CanManager::get_active_instance()->write_file_log(secStr);
        }
        send_section_frame();
    }

    void AmazoneBridge::on_aog_mode_received(bool autoMode)
    {
        lastAogPacketTimestampMs = get_current_time_ms();
        autoControlMode = autoMode;
    }

    void AmazoneBridge::on_aog_speed_received(float speedKph)
    {
        lastGpsSpeedKph = speedKph;
    }

    bool AmazoneBridge::is_aog_connected() const
    {
        if (lastAogPacketTimestampMs == 0) return false;
        return (get_current_time_ms() - lastAogPacketTimestampMs) < 2500;
    }

    void AmazoneBridge::compute_can_payload(std::uint8_t &b2, std::uint8_t &b3)
    {
        b2 = 0x00;
        b3 = 0x00;

        // Byte 2: Sections 1-8
        for (int i = 0; i < 8; ++i)
        {
            if (i < static_cast<int>(numSections) && sections[i])
            {
                b2 |= static_cast<std::uint8_t>(1 << i);
            }
        }

        // Byte 3: Sections 9-13
        for (int i = 8; i < 13; ++i)
        {
            if (i < static_cast<int>(numSections) && sections[i])
            {
                b3 |= static_cast<std::uint8_t>(1 << (i - 8));
            }
        }

        // Bit 7 of Byte 3 is the Main Switch (Master Valve)
        if (masterValveOpen)
        {
            b3 |= 0x80;
        }
    }

    bool AmazoneBridge::send_address_claim()
    {
        try
        {
            // AmaClick Address Claim: CAN ID 0x18EEFFCE, Payload 8 bytes
            isobus::CANMessageFrame txFrame;
            txFrame.identifier = 0x18EEFFCE;
            txFrame.isExtendedFrame = true;
            txFrame.dataLength = 8;
            txFrame.channel = 0;

            const std::uint8_t claimPayload[8] = { 0x28, 0xEC, 0x44, 0x0C, 0x00, 0x80, 0x1A, 0x20 };
            std::memcpy(txFrame.data, claimPayload, 8);

            bool sent = isobus::send_can_message_frame_to_hardware(txFrame);
            if (sent)
            {
                lastClaimTimestampMs = get_current_time_ms();
                totalTxFrames++;
            }
            return sent;
        }
        catch (...)
        {
            return false;
        }
    }

    bool AmazoneBridge::send_section_frame()
    {
        if (!bridgeEnabled) return false;

        std::uint8_t b2 = 0;
        std::uint8_t b3 = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(dataMutex);
            compute_can_payload(b2, b3);
            cachedByte2 = b2;
            cachedByte3 = b3;
        }

        try
        {
            // AmaClick Section Control Data: CAN ID 0x18E6FFCE
            isobus::CANMessageFrame txFrame;
            txFrame.identifier = 0x18E6FFCE;
            txFrame.isExtendedFrame = true;
            txFrame.dataLength = 8;
            txFrame.channel = 0;

            txFrame.data[0] = 0x21;
            txFrame.data[1] = 0xFA;
            txFrame.data[2] = b2;
            txFrame.data[3] = b3;
            txFrame.data[4] = 0x00;
            txFrame.data[5] = 0x00;
            txFrame.data[6] = 0x01;
            txFrame.data[7] = byte7Val; // 0x07 default

            bool sent = isobus::send_can_message_frame_to_hardware(txFrame);
            if (sent)
            {
                lastTxTimestampMs = get_current_time_ms();
                totalTxFrames++;

                char hexBuf[64];
                snprintf(hexBuf, sizeof(hexBuf), "%02X %02X %02X %02X %02X %02X %02X %02X",
                         txFrame.data[0], txFrame.data[1], txFrame.data[2], txFrame.data[3],
                         txFrame.data[4], txFrame.data[5], txFrame.data[6], txFrame.data[7]);
                lastFrameHex = hexBuf;
            }
            return sent;
        }
        catch (...)
        {
            return false;
        }
    }

    void AmazoneBridge::update()
    {
        if (!bridgeEnabled) return;

        std::uint32_t now = get_current_time_ms();

        // Safety watchdog: if in AUTO mode and AOG packets stop for > 2500 ms, shut off all sections!
        if (autoControlMode && lastAogPacketTimestampMs > 0 && (now - lastAogPacketTimestampMs) > 2500)
        {
            bool wasOn = false;
            {
                std::lock_guard<std::recursive_mutex> lock(dataMutex);
                wasOn = masterValveOpen;
                for (std::size_t i = 0; i < numSections; ++i)
                {
                    sections[i] = false;
                }
                masterValveOpen = false;
            }
            if (wasOn)
            {
                send_section_frame();
            }
        }

        // Periodic Address Claim (every 10 seconds or once at start)
        if (lastClaimTimestampMs == 0 || (now - lastClaimTimestampMs) >= 10000)
        {
            send_address_claim();
        }

        // Periodic Section Frame transmission (e.g. 5 Hz / every 200 ms)
        if (now - lastTxTimestampMs >= txIntervalMs)
        {
            send_section_frame();
        }
    }

    void AmazoneBridge::render_embedded()
    {
        // Snapshot section states under lock to guarantee UI thread safety
        std::vector<bool> localSections;
        std::uint8_t localNumSec = 9;
        bool localMaster = false;
        {
            std::lock_guard<std::recursive_mutex> lock(dataMutex);
            localSections = sections;
            localNumSec = numSections;
            localMaster = masterValveOpen;
        }

        // Header & Status Bar
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.00f, 0.48f, 0.24f, 1.0f)); // Amazone Green
        ImGui::TextColored(ImVec4(0.0f, 0.95f, 0.45f, 1.0f), "AMAZONE CAN AMACLICK EMULATOR");
        ImGui::SameLine();
        ImGui::TextDisabled("| Kontroler 9 Sekcji & Zaworu Glownego dla Amatron+ / Amatron 3");
        ImGui::Separator();

        // Global Enable Toggle & Badges
        bool en = bridgeEnabled;
        if (ImGui::Checkbox("Aktywuj Nadawanie CAN Amazone", &en))
        {
            set_enabled(en);
            TerminalSettings::get_instance().play_touch_beep();
        }
        ImGui::SameLine();

        // Link Badges
        bool aogOk = is_aog_connected();
        if (aogOk)
        {
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "[ UDP AOG: POLACZONO (%.1f km/h) ]", lastGpsSpeedKph);
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "[ UDP AOG: OCZEKIWANIE... ]");
        }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "[ TX: %llu ramek ]", static_cast<unsigned long long>(totalTxFrames));
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.2f, 1.0f), "[ LOG: can_traffic.log ]");
        ImGui::SameLine();
        if (ImGui::SmallButton("Wyczysc Log"))
        {
            if (CanManager::get_active_instance()) CanManager::get_active_instance()->clear_file_log();
            TerminalSettings::get_instance().play_touch_beep();
        }

        ImGui::Spacing();

        // Mode Switcher: AUTO vs MANUAL
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        ImGui::Text("Tryb Sterowania:");
        ImGui::SameLine();

        bool isAuto = autoControlMode;
        if (isAuto)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.60f, 0.24f, 1.0f));
            if (ImGui::Button("  ● TRYB AUTOMATYCZNY (AgOpenGPS)  ", ImVec2(270, 34)))
            {
                set_auto_mode(false);
                TerminalSettings::get_instance().play_touch_beep();
            }
            ImGui::PopStyleColor();
        }
        else
        {
            if (ImGui::Button("  Przelacz na AUTOMAT (AOG)  ", ImVec2(250, 34)))
            {
                set_auto_mode(true);
                TerminalSettings::get_instance().play_touch_beep();
            }
        }

        ImGui::SameLine();
        if (!isAuto)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.45f, 0.10f, 1.0f)); // Orange
            if (ImGui::Button("  ● TRYB RECZNY (Pulpit Dotykowy)  ", ImVec2(270, 34)))
            {
                set_auto_mode(true);
                TerminalSettings::get_instance().play_touch_beep();
            }
            ImGui::PopStyleColor();
        }
        else
        {
            if (ImGui::Button("  Przelacz na RECZNY  ", ImVec2(210, 34)))
            {
                set_auto_mode(false);
                TerminalSettings::get_instance().play_touch_beep();
            }
        }
        ImGui::PopStyleVar();

        ImGui::Separator();
        ImGui::Spacing();

        // --- MASTER SWITCH (ZAWÓR GŁÓWNY) ---
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
        if (localMaster)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.75f, 0.25f, 1.0f)); // Bright green
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.85f, 0.30f, 1.0f));
            if (ImGui::Button(">>>  GLOWNY ZAWOR (MASTER): OTWARTY (PRYSKA)  <<<", ImVec2(-1, 52)))
            {
                toggle_master_valve();
                TerminalSettings::get_instance().play_touch_beep();
            }
            ImGui::PopStyleColor(2);
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.12f, 0.12f, 1.0f)); // Bright red
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.18f, 0.18f, 1.0f));
            if (ImGui::Button("---  GLOWNY ZAWOR (MASTER): ZAMKNIETY (STOP)  ---", ImVec2(-1, 52)))
            {
                toggle_master_valve();
                TerminalSettings::get_instance().play_touch_beep();
            }
            ImGui::PopStyleColor(2);
        }
        ImGui::PopStyleVar();

        ImGui::Spacing();

        // Quick Controls
        if (ImGui::Button("Wszystkie WLACZ", ImVec2(140, 30)))
        {
            set_all_sections(true);
            TerminalSettings::get_instance().play_touch_beep();
        }
        ImGui::SameLine();
        if (ImGui::Button("Wszystkie WYLACZ", ImVec2(140, 30)))
        {
            set_all_sections(false);
            TerminalSettings::get_instance().play_touch_beep();
        }
        ImGui::SameLine();
        if (ImGui::Button("Odwroc Sekcje", ImVec2(130, 30)))
        {
            invert_all_sections();
            TerminalSettings::get_instance().play_touch_beep();
        }
        ImGui::SameLine();
        ImGui::TextDisabled(isAuto ? "(W trybie AUTO stany sa automatycznie odbierane z AgOpenGPS)" : "(W trybie RECZNYM dotknij sekcji ponizej aby otworzyc/zamknac)");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "BELKA POLOWA OPRYSKIWACZA (%u SEKCJI):", static_cast<unsigned>(localNumSec));

        // --- 9 INTERACTIVE SECTION BUTTONS ---
        float availW = ImGui::GetContentRegionAvail().x;
        int nSec = localNumSec;
        float btnWidth = std::max<float>(55.0f, (availW - (nSec - 1) * 8.0f) / nSec);

        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
        for (int i = 0; i < nSec; ++i)
        {
            if (i > 0) ImGui::SameLine(0.0f, 8.0f);

            bool sOn = (i < static_cast<int>(localSections.size())) ? localSections[i] : false;
            char btnId[32];
            snprintf(btnId, sizeof(btnId), "%s\nS%d\n%s###Sec_%d",
                     sOn ? "[ON]" : "[OFF]",
                     i + 1,
                     sOn ? "OTWARTA" : "ZAMKN.",
                     i);

            if (sOn)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.70f, 0.22f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.82f, 0.28f, 1.0f));
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.22f, 0.25f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.33f, 0.38f, 1.0f));
            }

            if (ImGui::Button(btnId, ImVec2(btnWidth, 80)))
            {
                toggle_section(i);
                TerminalSettings::get_instance().play_touch_beep();
            }
            ImGui::PopStyleColor(2);
        }
        ImGui::PopStyleVar();

        ImGui::Spacing();
        ImGui::Separator();

        // Tabs: Live CAN Frame & Diagnostic / Configuration / Pinout & Adapter Wiring
        if (ImGui::BeginTabBar("AmazoneBridgeTabs"))
        {
            if (ImGui::BeginTabItem("Podglad Ramki CAN & Diagnostyka"))
            {
                ImGui::Spacing();
                ImGui::Text("Emulowany sterownik: ");
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "AMACLICK Switchbox (Adres 0xCE)");

                ImGui::Text("Adres Claiming (0x18EEFFCE): ");
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "28 EC 44 0C 00 80 1A 20");
                ImGui::SameLine();
                if (ImGui::SmallButton("Wyslij Claim Teraz"))
                {
                    send_address_claim();
                    TerminalSettings::get_instance().play_touch_beep();
                }

                ImGui::Text("Ramka Sekcji (0x18E6FFCE): ");
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%s", lastFrameHex.empty() ? "(jeszcze nie wyslano)" : lastFrameHex.c_str());

                ImGui::Spacing();
                ImGui::Text("Struktura bajtow (Protokol Amatron CAN):");
                ImGui::BulletText("Bajt 0: 0x21 (Staly)");
                ImGui::BulletText("Bajt 1: 0xFA (Staly)");
                ImGui::BulletText("Bajt 2: 0x%02X -> Sekcje 1-8 bitowo (Bit 0 = S1 ... Bit 7 = S8)", cachedByte2);
                ImGui::BulletText("Bajt 3: 0x%02X -> Bit 0 = S9, Bity 1-4 = S10-S13, Bit 7 = Glowny Zawor (Master = %d)", cachedByte3, localMaster ? 1 : 0);
                ImGui::BulletText("Bajt 4-5: 0x00 0x00");
                ImGui::BulletText("Bajt 6: 0x01");
                ImGui::BulletText("Bajt 7: 0x%02X (Wersja AMATRON)", byte7Val);

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Ustawienia Protokolu"))
            {
                ImGui::Spacing();
                int curSections = localNumSec;
                if (ImGui::SliderInt("Liczba sekcji belki opryskiwacza", &curSections, 1, 13))
                {
                    set_num_sections(static_cast<std::uint8_t>(curSections));
                }

                int curRate = txIntervalMs;
                if (ImGui::SliderInt("Czestotliwosc nadawania (ms)", &curRate, 50, 1000, "%d ms (5 Hz = 200 ms)"))
                {
                    set_tx_rate_ms(curRate);
                }

                int b7 = byte7Val;
                const char* b7Names[] = { "0x07 (Standardowy Amatron+ / Amatron 3)", "0x09 (Alternatywny kod Amatron)" };
                int b7Idx = (b7 == 0x09) ? 1 : 0;
                if (ImGui::Combo("Wartosc Bajtu 7 w ramce", &b7Idx, b7Names, IM_ARRAYSIZE(b7Names)))
                {
                    set_byte7(b7Idx == 1 ? 0x09 : 0x07);
                }

                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Schemat Przelotki i Polaczenia"))
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "JAK PODLACZYC PCAN-USB DO OPRYSKIWACZA AMAZONE:");
                ImGui::TextWrapped(
                    "Aby sterowac opryskiwaczem Amazone (np. UF, UG, UX) wyposazonym w komputer AMATRON+ lub AMATRON 3 "
                    "bez fabrycznego ISOBUS, wpinamy przejsciowke ('przelotke') w wiazke magistrali CAN komputera AMATRON "
                    "(gniazdo AmaClick / wtyk CAN ciagnik-komputer).\n\n"
                    "Nasz program emuluje skrzynke AmaClick wysylajac ramki CAN 250 kbps bezposrednio do komputera Amazone.");

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f), "Rozpiska pinow wtyku DB9 (PEAK PCAN-USB):");
                ImGui::BulletText("PIN 7: CAN_High  ---> Podlacz do przewodu CAN-H w instalacji Amazone");
                ImGui::BulletText("PIN 2: CAN_Low   ---> Podlacz do przewodu CAN-L w instalacji Amazone");
                ImGui::BulletText("PIN 3 lub 5: GND ---> Podlacz do masy pojazdu / przewodu GND CAN");
                ImGui::BulletText("Rezystor terminujacy: 120 Ohm pomiedzy CAN_H i CAN_L (jesli przewod jest odgalezieniem)");

                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "Tryb pracy z AgOpenGPS:");
                ImGui::BulletText("1. W AgOpenGPS skonfiguruj sekcje opryskiwacza (9 sekcji).");
                ImGui::BulletText("2. Wlacz Section Control w AOG - stany sekcji sa natychmiast odbierane przez UDP (port 8888).");
                ImGui::BulletText("3. Nasz terminal tlumaczy je w locie na kody AmaClick i wysyla przez PCAN do opryskiwacza.");

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::PopStyleColor(); // ImGuiCol_Header
    }

    void AmazoneBridge::render_window(bool *p_open)
    {
        if (p_open && !*p_open) return;

        ImGui::SetNextWindowSize(ImVec2(840, 650), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Amazone CAN Section Control (Non-ISOBUS Amatron)###AmazoneBridgeWin", p_open))
        {
            ImGui::End();
            return;
        }

        render_embedded();

        ImGui::End();
    }
}
