// VTRenderer.cpp
// Full ISO 11783-6 VT object rendering engine using Dear ImGui draw lists.
// Supports: DataMask, AlarmMask, Container, Button, Key, SoftKeyMask,
//           OutputString, OutputNumber, OutputRectangle, OutputLine,
//           OutputEllipse, OutputPolygon, OutputLinearBarGraph, OutputArchedBarGraph,
//           OutputMeter, PictureGraphic (colour-mapped), InputString, InputNumber,
//           InputBoolean, InputList, WorkingSet.
// Attributes resolved: FontAttributes, LineAttributes, FillAttributes.
// Justification, LineSuppression, LineDirection, Button options all handled.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "VTRenderer.hpp"
#include "TerminalSettings.hpp"
#include "AudioBeeper.hpp"
#include "CanManager.hpp"
#include "TaskControllerManager.hpp"
#include "isobus/isobus/isobus_virtual_terminal_objects.hpp"
#include "isobus/isobus/can_network_manager.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace isobus_vt
{
    // ─── ISO 11783-6 VT colour palette (256 entries; indices 0-15 fixed) ─────────
    // Indices 16-231: 6x6x6 colour cube; 232-255: greyscale ramp.
    static ImU32 iso_palette_color(std::uint8_t idx)
    {
        // ISO 11783-6: Index 254 is transparent colour
        if (idx == 254) return IM_COL32(0, 0, 0, 0);

        // Fixed 16-colour VT palette
        static const ImU32 fixed[16] = {
            IM_COL32(0,   0,   0,   255), // 0  Black
            IM_COL32(255, 255, 255, 255), // 1  White
            IM_COL32(0,   153, 0,   255), // 2  Green
            IM_COL32(0,   153, 153, 255), // 3  Teal
            IM_COL32(153, 0,   0,   255), // 4  Maroon
            IM_COL32(153, 0,   153, 255), // 5  Purple
            IM_COL32(153, 153, 0,   255), // 6  Olive
            IM_COL32(204, 204, 204, 255), // 7  Silver
            IM_COL32(102, 102, 102, 255), // 8  Grey
            IM_COL32(0,   0,   255, 255), // 9  Blue
            IM_COL32(0,   255, 0,   255), // 10 Lime
            IM_COL32(0,   255, 255, 255), // 11 Cyan
            IM_COL32(255, 0,   0,   255), // 12 Red
            IM_COL32(255, 0,   255, 255), // 13 Magenta
            IM_COL32(255, 255, 0,   255), // 14 Yellow
            IM_COL32(0,   0,   128, 255), // 15 Navy
        };
        if (idx < 16) return fixed[idx];

        // Indices 16-231: 6x6x6 colour cube (same mapping as xterm-256)
        if (idx < 232)
        {
            int i  = idx - 16;
            int r  = (i / 36) % 6;
            int g  = (i / 6)  % 6;
            int b  =  i       % 6;
            auto c = [](int v) -> int { return v ? 55 + v * 40 : 0; };
            return IM_COL32(c(r), c(g), c(b), 255);
        }

        // Indices 232-255: greyscale ramp
        int level = 8 + (idx - 232) * 10;
        return IM_COL32(level, level, level, 255);
    }

    // ─────────────────────────────────────────────────────────────────────────────

    VTRenderer::VTRenderer(std::shared_ptr<CustomVTServer> vtServer) :
        vtServer(vtServer)
    {
        load_settings();
    }

    void VTRenderer::save_settings()
    {
        auto &ts = TerminalSettings::get_instance();
        auto &cfg = ts.get_config();

        cfg.vtVersion = static_cast<int>(vtServer->get_configured_version());
        cfg.dataMaskSize = vtServer->get_configured_mask_size();
        cfg.dataMaskWidth = vtServer->get_configured_mask_size();
        cfg.dataMaskHeight = vtServer->get_configured_mask_size();
        cfg.skDescWidth = vtServer->get_configured_sk_width();
        cfg.skDescHeight = vtServer->get_configured_sk_height();
        cfg.skColumns = vtServer->get_configured_sk_columns();
        cfg.skRows = vtServer->get_configured_sk_rows();
        cfg.vtNumber = vtServer->get_configured_vt_number();
        cfg.zoom = maskScale;

        cfg.langCode = langCode;
        cfg.countryCode = countryCode;
        cfg.unitArea = unitArea;
        cfg.unitDate = unitDate;
        cfg.unitDecimal = unitDecimal;
        cfg.unitDistance = unitDistance;
        cfg.unitForce = unitForce;
        cfg.unitGeneric = unitGeneric;
        cfg.unitMass = unitMass;
        cfg.unitPressure = unitPressure;
        cfg.unitTemperature = unitTemperature;
        cfg.unitTime = unitTime;
        cfg.unitVolume = unitVolume;

        ts.save();
    }

    void VTRenderer::load_settings()
    {
        auto &ts = TerminalSettings::get_instance();
        ts.load();
        const auto &cfg = ts.get_config();

        vtServer->set_vt_version(static_cast<isobus::VirtualTerminalBase::VTVersion>(cfg.vtVersion));
        vtServer->set_data_mask_size(static_cast<std::uint16_t>(cfg.dataMaskSize));
        vtServer->set_sk_desc_width(static_cast<std::uint8_t>(cfg.skDescWidth));
        vtServer->set_sk_desc_height(static_cast<std::uint8_t>(cfg.skDescHeight));
        vtServer->set_sk_columns(static_cast<std::uint8_t>(cfg.skColumns));
        vtServer->set_sk_rows(static_cast<std::uint8_t>(cfg.skRows));
        vtServer->set_vt_number(static_cast<std::uint8_t>(cfg.vtNumber));
        maskScale = cfg.zoom;

        if (cfg.langCode.length() >= 2)
        {
            langCode[0] = cfg.langCode[0];
            langCode[1] = cfg.langCode[1];
            langCode[2] = '\0';
        }
        if (cfg.countryCode.length() >= 2)
        {
            countryCode[0] = cfg.countryCode[0];
            countryCode[1] = cfg.countryCode[1];
            countryCode[2] = '\0';
        }

        unitArea = cfg.unitArea;
        unitDate = cfg.unitDate;
        unitDecimal = cfg.unitDecimal;
        unitDistance = cfg.unitDistance;
        unitForce = cfg.unitForce;
        unitGeneric = cfg.unitGeneric;
        unitMass = cfg.unitMass;
        unitPressure = cfg.unitPressure;
        unitTemperature = cfg.unitTemperature;
        unitTime = cfg.unitTime;
        unitVolume = cfg.unitVolume;
    }

    ImU32 VTRenderer::get_imgui_color(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws,
                                      std::uint8_t colorIndex,
                                      ImU32 defaultColor)
    {
        if (colorIndex == 254) return IM_COL32(0, 0, 0, 0);
        if (ws)
        {
            auto col = ws->get_colour(colorIndex);
            return IM_COL32(
                static_cast<int>(col.r * 255.0f),
                static_cast<int>(col.g * 255.0f),
                static_cast<int>(col.b * 255.0f),
                255
            );
        }
        return iso_palette_color(colorIndex);
        (void)defaultColor;
    }

    // ─── Attribute helpers ────────────────────────────────────────────────────────

    // Resolve fill colour + type from a FillAttributes object.
    // Returns false when hasFill should be set to false (NoFill).
    static bool resolve_fill(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws,
                             std::uint16_t fillAttrId,
                             ImU32 &outColor)
    {
        if (fillAttrId == isobus::NULL_OBJECT_ID) return false;
        auto attr = std::dynamic_pointer_cast<isobus::FillAttributes>(ws->get_object_by_id(fillAttrId));
        if (!attr) return false;

        using FT = isobus::FillAttributes::FillType;
        switch (attr->get_type())
        {
            case FT::NoFill:
                return false;
            case FT::FillWithLineColor:
                // Use line colour — caller must supply separately; here just use palette colour 0
                outColor = VTRenderer::get_imgui_color_static(ws, attr->get_background_color());
                return true;
            case FT::FillWithSpecifiedColorInFillColorAttribute:
                outColor = VTRenderer::get_imgui_color_static(ws, attr->get_background_color());
                return true;
            case FT::FillWithPatternGivenByFillPatternAttribute:
                // Pattern fill not supported in ImGui — use solid colour
                outColor = VTRenderer::get_imgui_color_static(ws, attr->get_background_color());
                return true;
        }
        return false;
    }

    static void resolve_line(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws,
                             std::uint16_t lineAttrId,
                             ImU32 &outColor,
                             float &outWidth)
    {
        if (lineAttrId == isobus::NULL_OBJECT_ID) return;
        auto attr = std::dynamic_pointer_cast<isobus::LineAttributes>(ws->get_object_by_id(lineAttrId));
        if (!attr) return;
        outColor = VTRenderer::get_imgui_color_static(ws, attr->get_background_color());
        outWidth = static_cast<float>(attr->get_width());
        if (outWidth < 1.0f) outWidth = 1.0f;
    }

    static void resolve_font(std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws,
                             std::uint16_t fontAttrId,
                             ImU32 &outColor,
                             float &outHeightPx)
    {
        outHeightPx = 14.0f;
        if (fontAttrId == isobus::NULL_OBJECT_ID) return;
        auto attr = std::dynamic_pointer_cast<isobus::FontAttributes>(ws->get_object_by_id(fontAttrId));
        if (!attr) return;
        outColor = VTRenderer::get_imgui_color_static(ws, attr->get_colour());
        float fh = static_cast<float>(attr->get_font_height_pixels());
        if (fh >= 6.0f)
        {
            outHeightPx = fh;
        }
    }

    // ─── Static colour helper (for use inside static functions) ──────────────────

    /* static */ ImU32 VTRenderer::get_imgui_color_static(
        std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws,
        std::uint8_t colorIndex)
    {
        if (colorIndex == 254) return IM_COL32(0, 0, 0, 0);
        if (ws)
        {
            auto col = ws->get_colour(colorIndex);
            return IM_COL32(
                static_cast<int>(col.r * 255.0f),
                static_cast<int>(col.g * 255.0f),
                static_cast<int>(col.b * 255.0f),
                255
            );
        }
        return iso_palette_color(colorIndex);
    }

    // ─── Text rendering helper with ISO 11783-6 justification & word wrapping ──
    static void draw_text_justified(ImDrawList *dl,
                                    const std::string &text,
                                    ImVec2 pos,
                                    float w, float h,
                                    ImU32 col,
                                    float fontHeightPx = 14.0f,
                                    float scale = 1.0f,
                                    isobus::TextualVTObject::HorizontalJustification hjust
                                        = isobus::TextualVTObject::HorizontalJustification::PositionLeft,
                                    isobus::TextualVTObject::VerticalJustification vjust
                                        = isobus::TextualVTObject::VerticalJustification::PositionTop)
    {
        if (text.empty()) return;

        ImGuiIO &io = ImGui::GetIO();
        ImFont *font = ImGui::GetFont();
        float targetFontSize = std::max(8.0f, fontHeightPx * scale);
        if (h > 0.0f && targetFontSize > h * 1.05f)
            targetFontSize = std::max(8.0f, h * 0.95f);

        // Select the crisp rasterized font closest to target size to avoid blur
        if (io.Fonts && io.Fonts->Fonts.Size >= 3)
        {
            if (targetFontSize >= 36.0f)
            {
                font = io.Fonts->Fonts[2]; // 56px TrueType bold
            }
            else if (targetFontSize >= 18.0f)
            {
                font = io.Fonts->Fonts[1]; // 22px TrueType bold
            }
            else
            {
                font = io.Fonts->Fonts[0]; // 15px TrueType standard
            }
        }

        // First split lines by newline '\n'
        std::vector<std::string> rawLines;
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            rawLines.push_back(line);
        }
        if (rawLines.empty()) rawLines.push_back(text);

        // Word-wrap each line if it exceeds box width w
        std::vector<std::string> lines;
        for (const auto &rl : rawLines)
        {
            if (w <= 0.0f || font->CalcTextSizeA(targetFontSize, FLT_MAX, -1.0f, rl.c_str()).x <= w)
            {
                lines.push_back(rl);
                continue;
            }

            std::istringstream wsStream(rl);
            std::string word;
            std::string currentLine;
            while (wsStream >> word)
            {
                std::string candidate = currentLine.empty() ? word : (currentLine + " " + word);
                if (font->CalcTextSizeA(targetFontSize, FLT_MAX, -1.0f, candidate.c_str()).x <= w)
                {
                    currentLine = candidate;
                }
                else
                {
                    if (!currentLine.empty())
                        lines.push_back(currentLine);
                    currentLine = word;
                }
            }
            if (!currentLine.empty())
                lines.push_back(currentLine);
        }
        if (lines.empty()) lines = rawLines;

        float lineH = targetFontSize * 1.15f;
        float totalH = lineH * static_cast<float>(lines.size());

        float startY = pos.y;
        switch (vjust)
        {
            case isobus::TextualVTObject::VerticalJustification::PositionTop:
                startY = pos.y;
                break;
            case isobus::TextualVTObject::VerticalJustification::PositionMiddle:
                startY = pos.y + (h - totalH) * 0.5f;
                break;
            case isobus::TextualVTObject::VerticalJustification::PositionBottom:
                startY = pos.y + h - totalH;
                break;
            default:
                startY = pos.y;
                break;
        }
        if (h > 0.0f && startY < pos.y) startY = pos.y;

        for (auto &ln : lines)
        {
            ImVec2 ts = font->CalcTextSizeA(targetFontSize, FLT_MAX, -1.0f, ln.c_str());

            float startX = pos.x;
            switch (hjust)
            {
                case isobus::TextualVTObject::HorizontalJustification::PositionLeft:
                    startX = pos.x;
                    break;
                case isobus::TextualVTObject::HorizontalJustification::PositionMiddle:
                    startX = pos.x + std::max(0.0f, (w - ts.x) * 0.5f);
                    break;
                case isobus::TextualVTObject::HorizontalJustification::PositionRight:
                    startX = pos.x + std::max(0.0f, w - ts.x);
                    break;
                default:
                    startX = pos.x;
                    break;
            }

            dl->AddText(font, targetFontSize, ImVec2(startX, startY), col, ln.c_str());
            startY += lineH;
        }
    }

    // ─── PictureGraphic software rasteriser ──────────────────────────────────────
    static void draw_picture_graphic(ImDrawList *dl,
                                     std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws,
                                     std::shared_ptr<isobus::PictureGraphic> pic,
                                     ImVec2 pos,
                                     float displayW,
                                     float displayH)
    {
        if (!pic) return;
        const auto &raw = pic->get_raw_data();
        if (raw.empty()) return;

        std::uint16_t actualW = pic->get_actual_width();
        std::uint16_t actualH = pic->get_actual_height();
        if (displayW <= 0.0f) displayW = static_cast<float>(pic->get_width());
        if (displayH <= 0.0f) displayH = static_cast<float>(pic->get_height());
        if (actualW == 0 || actualH == 0 || displayW <= 0.0f || displayH <= 0.0f) return;

        float pixW = displayW / static_cast<float>(actualW);
        float pixH = displayH / static_cast<float>(actualH);

        bool isTransparent = pic->get_option(isobus::PictureGraphic::Options::Transparent);
        std::uint8_t transpCol = pic->get_transparency_colour();

        auto fmt = pic->get_format();
        std::uint32_t idx = 0;
        for (std::uint16_t y = 0; y < actualH && idx < static_cast<std::uint32_t>(raw.size()); ++y)
        {
            for (std::uint16_t x = 0; x < actualW; ++x)
            {
                std::uint8_t palIdx = 0;
                if (fmt == isobus::PictureGraphic::Format::Monochrome)
                {
                    std::uint32_t bitIdx = y * actualW + x;
                    std::uint32_t byteIdx = bitIdx / 8;
                    if (byteIdx >= raw.size()) break;
                    palIdx = (raw[byteIdx] >> (7 - (bitIdx % 8))) & 1;
                }
                else if (fmt == isobus::PictureGraphic::Format::FourBitColour)
                {
                    std::uint32_t nibbleIdx = y * actualW + x;
                    std::uint32_t byteIdx   = nibbleIdx / 2;
                    if (byteIdx >= raw.size()) break;
                    palIdx = (nibbleIdx % 2 == 0)
                                ? (raw[byteIdx] >> 4) & 0x0F
                                : raw[byteIdx] & 0x0F;
                }
                else // EightBitColour
                {
                    if (idx >= raw.size()) break;
                    palIdx = raw[idx++];
                }

                if (isTransparent && palIdx == transpCol) continue;

                ImU32 c = VTRenderer::get_imgui_color_static(ws, palIdx);
                float px = pos.x + static_cast<float>(x) * pixW;
                float py = pos.y + static_cast<float>(y) * pixH;
                dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pixW + 0.5f, py + pixH + 0.5f), c);
            }
            if (fmt == isobus::PictureGraphic::Format::Monochrome)
                idx = (static_cast<std::uint32_t>(y + 1) * actualW + 7) / 8;
            else if (fmt == isobus::PictureGraphic::Format::FourBitColour)
                idx = (static_cast<std::uint32_t>(y + 1) * actualW + 1) / 2;
        }
    }

    // ─── Meter drawing helper ─────────────────────────────────────────────────────
    static void draw_meter(ImDrawList *dl,
                           std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws,
                           std::shared_ptr<isobus::OutputMeter> meter,
                           ImVec2 pos, float w, float h)
    {
        if (!meter || w <= 0.0f || h <= 0.0f) return;

        float cx = pos.x + w * 0.5f;
        float cy = pos.y + h * 0.5f;
        float r  = std::min(w, h) * 0.5f - 2.0f;
        if (r <= 2.0f) return;

        ImU32 borderCol = VTRenderer::get_imgui_color_static(ws, meter->get_border_colour());
        ImU32 needleCol = VTRenderer::get_imgui_color_static(ws, meter->get_needle_colour());
        ImU32 tickCol   = VTRenderer::get_imgui_color_static(ws, meter->get_arc_and_tick_colour());

        // ISO 11783-6 OutputMeter:
        // Angles are stored in units of 2 degrees counterclockwise from positive X-axis (3 o'clock).
        // 0° = 3 o'clock (+X, 0 Y)
        // 90° = 12 o'clock (0 X, -Y in screen coords)
        // 180° = 9 o'clock (-X, 0 Y)
        // 270° = 6 o'clock (0 X, +Y in screen coords)
        float startVtAngleDeg = static_cast<float>(meter->get_start_angle()) * 2.0f;
        float endVtAngleDeg   = static_cast<float>(meter->get_end_angle())   * 2.0f;
        bool clockwise = meter->get_option(isobus::OutputMeter::Options::DeflectionDirection);

        if (endVtAngleDeg < startVtAngleDeg)
        {
            endVtAngleDeg += 360.0f;
        }

        auto calcAngle = [startVtAngleDeg, endVtAngleDeg, clockwise](float frac) -> float {
            float theta = frac * (startVtAngleDeg - endVtAngleDeg);
            return clockwise ? (endVtAngleDeg + theta) : (endVtAngleDeg - theta);
        };

        // Convert ISO 11783-6 angle in degrees (CCW from positive X-axis) to screen coordinates
        auto toScreenPos = [cx, cy](float angleDeg, float radius) -> ImVec2 {
            float rad = angleDeg * static_cast<float>(M_PI) / 180.0f;
            return ImVec2(cx + radius * std::cos(rad), cy - radius * std::sin(rad));
        };

        // Draw border circle
        if (meter->get_option(isobus::OutputMeter::Options::DrawBorder))
            dl->AddCircle(ImVec2(cx, cy), r, borderCol, 64, 1.5f);

        // Draw arc
        if (meter->get_option(isobus::OutputMeter::Options::DrawArc))
        {
            const int SEG = 64;
            ImVec2 prevPt = toScreenPos(calcAngle(0.0f), r);
            for (int i = 1; i <= SEG; ++i)
            {
                float t = static_cast<float>(i) / static_cast<float>(SEG);
                ImVec2 pt = toScreenPos(calcAngle(t), r);
                dl->AddLine(prevPt, pt, tickCol, 2.0f);
                prevPt = pt;
            }
        }

        // Draw ticks
        if (meter->get_option(isobus::OutputMeter::Options::DrawTicks))
        {
            int nTicks = static_cast<int>(meter->get_number_of_ticks());
            if (nTicks > 1)
            {
                float innerR = r * 0.82f;
                for (int i = 0; i < nTicks; ++i)
                {
                    float t = static_cast<float>(i) / static_cast<float>(nTicks - 1);
                    float a = calcAngle(t);
                    dl->AddLine(toScreenPos(a, innerR), toScreenPos(a, r), tickCol, 2.0f);
                }
            }
        }

        

        // Resolve current needle value (supporting NumberVariable reference)
        std::uint32_t needleVal = meter->get_value();
        if (meter->get_variable_reference() != isobus::NULL_OBJECT_ID && ws)
        {
            auto varObj = ws->get_object_by_id(meter->get_variable_reference());
            if (varObj && varObj->get_object_type() == isobus::VirtualTerminalObjectType::NumberVariable)
            {
                auto numVar = std::dynamic_pointer_cast<isobus::NumberVariable>(varObj);
                if (numVar) needleVal = numVar->get_value();
            }
        }

        // Draw needle
        float minV = static_cast<float>(meter->get_min_value());
        float maxV = static_cast<float>(meter->get_max_value());
        float curV = static_cast<float>(needleVal);
        float frac = (maxV > minV) ? (curV - minV) / (maxV - minV) : 0.0f;
        frac = std::max(0.0f, std::min(1.0f, frac));

        float needleAngle = calcAngle(frac);
        float needleLen   = r * 0.82f;

        ImVec2 needleTip = toScreenPos(needleAngle, needleLen);
        dl->AddLine(ImVec2(cx, cy), needleTip, needleCol, 2.5f);
        dl->AddCircleFilled(ImVec2(cx, cy), std::max(3.0f, r * 0.08f), needleCol);
    }

    // ─── Linear bar graph ─────────────────────────────────────────────────────────
    static void draw_linear_bar_graph(ImDrawList *dl,
                                      std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws,
                                      std::shared_ptr<isobus::OutputLinearBarGraph> bar,
                                      ImVec2 pos, float w, float h)
    {
        ImU32 bgCol   = IM_COL32(230, 230, 230, 255);
        ImU32 barCol  = VTRenderer::get_imgui_color_static(ws, bar->get_colour());
        ImU32 tickCol = barCol;

        // Background
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), bgCol, 2.0f);
        if (bar->get_option(isobus::OutputLinearBarGraph::Options::DrawBorder))
            dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), barCol, 2.0f, 0, 1.5f);

        float minV = static_cast<float>(bar->get_min_value());
        float maxV = static_cast<float>(bar->get_max_value());
        float val  = static_cast<float>(bar->get_value());
        float frac = (maxV > minV) ? (val - minV) / (maxV - minV) : 0.0f;
        frac = std::max(0.0f, std::min(1.0f, frac));

        bool horizontal = bar->get_option(isobus::OutputLinearBarGraph::Options::AxisOrientation);
        bool growsPos   = bar->get_option(isobus::OutputLinearBarGraph::Options::Direction);
        bool filled     = !bar->get_option(isobus::OutputLinearBarGraph::Options::BarGraphType);

        if (horizontal)
        {
            float barW = w * frac;
            float startX = growsPos ? pos.x : pos.x + w - barW;
            if (filled)
                dl->AddRectFilled(ImVec2(startX, pos.y), ImVec2(startX + barW, pos.y + h), barCol, 2.0f);
            else
            {
                float lineX = growsPos ? pos.x + barW : pos.x + w - barW;
                dl->AddLine(ImVec2(lineX, pos.y), ImVec2(lineX, pos.y + h), barCol, 2.0f);
            }
        }
        else
        {
            float barH = h * frac;
            float startY = growsPos ? pos.y + h - barH : pos.y;
            if (filled)
                dl->AddRectFilled(ImVec2(pos.x, startY), ImVec2(pos.x + w, startY + barH), barCol, 2.0f);
            else
            {
                float lineY = growsPos ? pos.y + h - barH : pos.y + barH;
                dl->AddLine(ImVec2(pos.x, lineY), ImVec2(pos.x + w, lineY), barCol, 2.0f);
            }
        }

        // Ticks
        if (bar->get_option(isobus::OutputLinearBarGraph::Options::DrawTicks))
        {
            int nTicks = static_cast<int>(bar->get_number_of_ticks());
            if (nTicks > 0)
            {
                for (int i = 1; i < nTicks; ++i)
                {
                    float t = static_cast<float>(i) / static_cast<float>(nTicks);
                    if (horizontal)
                    {
                        float tx = pos.x + w * t;
                        dl->AddLine(ImVec2(tx, pos.y), ImVec2(tx, pos.y + 6.0f), tickCol, 1.0f);
                        dl->AddLine(ImVec2(tx, pos.y + h - 6.0f), ImVec2(tx, pos.y + h), tickCol, 1.0f);
                    }
                    else
                    {
                        float ty = pos.y + h * t;
                        dl->AddLine(ImVec2(pos.x, ty), ImVec2(pos.x + 6.0f, ty), tickCol, 1.0f);
                        dl->AddLine(ImVec2(pos.x + w - 6.0f, ty), ImVec2(pos.x + w, ty), tickCol, 1.0f);
                    }
                }
            }
        }

        

        // Target line
        if (bar->get_option(isobus::OutputLinearBarGraph::Options::DrawTargetLine))
        {
            float tv   = static_cast<float>(bar->get_target_value());
            float tfrac = (maxV > minV) ? (tv - minV) / (maxV - minV) : 0.0f;
            tfrac = std::max(0.0f, std::min(1.0f, tfrac));
            ImU32 tlCol = VTRenderer::get_imgui_color_static(ws, bar->get_target_line_colour());
            if (horizontal)
            {
                float tx = pos.x + w * tfrac;
                dl->AddLine(ImVec2(tx, pos.y), ImVec2(tx, pos.y + h), tlCol, 2.0f);
            }
            else
            {
                float ty = pos.y + h * (1.0f - tfrac);
                dl->AddLine(ImVec2(pos.x, ty), ImVec2(pos.x + w, ty), tlCol, 2.0f);
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────

    void VTRenderer::render_terminal_window()
    {
        // Layout calculations & window size constraints
        maskScale = std::max(0.5f, std::min(3.0f, maskScale));
        float configMaskPx  = static_cast<float>(vtServer->get_configured_mask_size());
        const float DATA_W  = configMaskPx * maskScale;
        const float DATA_H  = configMaskPx * maskScale;
        const float WS_W    = 76.0f * maskScale;
        const float SK_W    = 95.0f * maskScale;
        const float PADDING = 12.0f;
        float neededW = PADDING * 2.0f + WS_W + 4.0f + DATA_W + 4.0f + SK_W + 30.0f;
        float neededH = DATA_H + PADDING * 2.0f + 85.0f;

        ImGui::SetNextWindowSizeConstraints(ImVec2(neededW, neededH), ImVec2(FLT_MAX, FLT_MAX));
        if (resetLayoutRequested)
        {
            ImGui::SetNextWindowPos(ImVec2(10.0f, 30.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(neededW, neededH), ImGuiCond_Always);
            ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
            resetLayoutRequested = false;
        }
        else
        {
            ImGui::SetNextWindowPos(ImVec2(10.0f, 38.0f), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(neededW, neededH), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowCollapsed(false, ImGuiCond_Appearing);
        }

        ImGui::Begin("ISOBUS Universal Terminal (VT Server)", nullptr,
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse);

        auto ws     = vtServer->get_active_ws();
        bool isWsReady = (ws != nullptr) &&
                         (ws->get_working_set_object() != nullptr) &&
                         (ws->get_object_pool_processing_state() != isobus::VirtualTerminalServerManagedWorkingSet::ObjectPoolProcessingThreadState::Running);

        // ISO 11783-6 Acoustic Alarm Beeper loop
        AudioBeeper::get_instance().update_alarm_loop(vtServer->is_alarm_active());

        auto maskId = isWsReady ? vtServer->get_active_data_mask_id() : isobus::NULL_OBJECT_ID;
        auto skId   = isWsReady ? vtServer->get_active_soft_key_mask_id() : isobus::NULL_OBJECT_ID;

        // Fallback 1: soft key mask from the active data mask object
        if (skId == isobus::NULL_OBJECT_ID && isWsReady && maskId != isobus::NULL_OBJECT_ID)
        {
            auto dm = std::dynamic_pointer_cast<isobus::DataMask>(ws->get_object_by_id(maskId));
            if (dm) skId = dm->get_soft_key_mask();
        }
        // Fallback 2: also check AlarmMask
        if (skId == isobus::NULL_OBJECT_ID && isWsReady && maskId != isobus::NULL_OBJECT_ID)
        {
            auto am = std::dynamic_pointer_cast<isobus::AlarmMask>(ws->get_object_by_id(maskId));
            if (am) skId = am->get_soft_key_mask();
        }
        // Fallback 3: scan entire object pool for any SoftKeyMask
        if (skId == isobus::NULL_OBJECT_ID && isWsReady)
        {
            const auto &tree = ws->get_object_tree();
            for (const auto &[id, obj] : tree)
            {
                if (obj && obj->get_object_type() == isobus::VirtualTerminalObjectType::SoftKeyMask)
                {
                    skId = obj->get_id();
                    break;
                }
            }
        }

        

        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();

        const auto &cfg = TerminalSettings::get_instance().get_config();

        // Outer bezel styling (Müller-Elektronik TOUCH800, Modern Glass, Minimalist, Rugged)
        float topHeaderH = 0.0f;
        ImU32 bezelBg = IM_COL32(30, 32, 38, 255);
        ImU32 bezelBorder = IM_COL32(65, 70, 82, 255);
        float bezelRounding = 10.0f;
        float bezelThickness = 2.0f;

        if (cfg.bezelStyle == 0) // Müller-Elektronik TOUCH800 / TOUCH1200
        {
            topHeaderH = 30.0f;
            bezelBg = IM_COL32(20, 23, 29, 255); // Müller signature matte graphite
            bezelBorder = IM_COL32(14, 88, 185, 255); // Müller cobalt blue border
            bezelRounding = 12.0f;
            bezelThickness = 2.5f;
        }
        else if (cfg.bezelStyle == 1) // Modern Glass
        {
            bezelBg = IM_COL32(18, 22, 28, 255);
            bezelBorder = IM_COL32(50, 95, 150, 220);
            bezelRounding = 14.0f;
            bezelThickness = 2.0f;
        }
        else if (cfg.bezelStyle == 2) // Borderless Minimal
        {
            bezelBg = IM_COL32(24, 26, 32, 255);
            bezelBorder = IM_COL32(45, 50, 62, 255);
            bezelRounding = 4.0f;
            bezelThickness = 1.0f;
        }

        ImVec2 bezelBR(origin.x + PADDING + WS_W + 4.0f + DATA_W + 4.0f + SK_W + PADDING,
                       origin.y + topHeaderH + DATA_H + PADDING * 2.0f);

        dl->AddRectFilled(origin, bezelBR, bezelBg, bezelRounding);
        dl->AddRect(origin, bezelBR, bezelBorder, bezelRounding, 0, bezelThickness);

        // Draw Müller-Elektronik TOUCH800 Top Header Bar
        if (cfg.bezelStyle == 0)
        {
            float barX = origin.x + PADDING;
            float barY = origin.y + 6.0f;
            float barW = WS_W + 4.0f + DATA_W + 4.0f + SK_W;
            float barH = 24.0f;

            // Bar background & blue line
            dl->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH), IM_COL32(14, 17, 23, 255), 4.0f);
            dl->AddLine(ImVec2(barX, barY + barH - 1.0f), ImVec2(barX + barW, barY + barH - 1.0f), IM_COL32(14, 88, 185, 255), 2.0f);

            // Left: ME Blue Badge & Logo
            dl->AddRectFilled(ImVec2(barX + 4.0f, barY + 3.0f), ImVec2(barX + 32.0f, barY + barH - 3.0f), IM_COL32(14, 88, 185, 255), 3.0f);
            dl->AddText(ImVec2(barX + 7.0f, barY + 4.0f), IM_COL32(255, 255, 255, 255), "ME");
            dl->AddText(ImVec2(barX + 38.0f, barY + 4.0f), IM_COL32(255, 255, 255, 255), "M\xc3\x9cLLER-ELEKTRONIK");
            dl->AddText(ImVec2(barX + 215.0f, barY + 4.0f), IM_COL32(14, 135, 250, 255), "TOUCH800");

            // Center: Active Application / Implement Model Badge
            std::string centerTitle = isWsReady ? "TITAN-XT 16000 ULTRA" : "SYSTEM STANDBY";
            ImVec2 cSz = ImGui::CalcTextSize(centerTitle.c_str());
            float cX = barX + (barW - cSz.x) * 0.5f;
            dl->AddRectFilled(ImVec2(cX - 8.0f, barY + 3.0f), ImVec2(cX + cSz.x + 8.0f, barY + barH - 3.0f), IM_COL32(28, 34, 46, 255), 3.0f);
            dl->AddRect(ImVec2(cX - 8.0f, barY + 3.0f), ImVec2(cX + cSz.x + 8.0f, barY + barH - 3.0f), IM_COL32(14, 88, 185, 180), 3.0f);
            dl->AddText(ImVec2(cX, barY + 4.0f), IM_COL32(220, 235, 255, 255), centerTitle.c_str());

            // Right: Status Badges & Clock
            float rx = barX + barW - 6.0f;

            // Live Time Clock
            time_t now = time(nullptr);
            tm ltm;
            localtime_s(&ltm, &now);
            char timeBuf[16];
            snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", ltm.tm_hour, ltm.tm_min, ltm.tm_sec);
            rx -= 55.0f;
            dl->AddText(ImVec2(rx, barY + 4.0f), IM_COL32(180, 195, 215, 255), timeBuf);

            // Voltage & Bus
            rx -= 50.0f;
            dl->AddText(ImVec2(rx, barY + 4.0f), IM_COL32(100, 210, 255, 255), "14.1 V");

            // Master Status
            rx -= 75.0f;
            dl->AddCircleFilled(ImVec2(rx + 6.0f, barY + 12.0f), 4.0f, IM_COL32(46, 204, 113, 255));
            dl->AddText(ImVec2(rx + 15.0f, barY + 4.0f), IM_COL32(220, 230, 240, 255), "MASTER");

            // TC-SC Status
            rx -= 70.0f;
            dl->AddCircleFilled(ImVec2(rx + 6.0f, barY + 12.0f), 4.0f, IM_COL32(243, 156, 18, 255));
            dl->AddText(ImVec2(rx + 15.0f, barY + 4.0f), IM_COL32(220, 230, 240, 255), "TC-SC");

            // GPS RTK Status
            rx -= 85.0f;
            dl->AddCircleFilled(ImVec2(rx + 6.0f, barY + 12.0f), 4.0f, IM_COL32(46, 204, 113, 255));
            dl->AddText(ImVec2(rx + 15.0f, barY + 4.0f), IM_COL32(220, 230, 240, 255), "GPS RTK");
        }

        ImVec2 wsPos, maskPos, skPos;
        ImVec2 wsSize(WS_W, DATA_H);
        ImVec2 maskSize(DATA_W, DATA_H);
        ImVec2 skSize(SK_W, DATA_H);

        float contentTop = origin.y + PADDING + topHeaderH;
        if (cfg.softKeyOnRight)
        {
            // Standard layout: WS Bar on Left -> DataMask in Middle -> SoftKeys on Right
            wsPos = ImVec2(origin.x + PADDING, contentTop);
            maskPos = ImVec2(wsPos.x + WS_W + 4.0f, contentTop);
            skPos = ImVec2(maskPos.x + DATA_W + 4.0f, contentTop);
        }
        else
        {
            // Left-handed layout: SoftKeys on Left -> WS Bar in Middle -> DataMask on Right
            skPos = ImVec2(origin.x + PADDING, contentTop);
            wsPos = ImVec2(skPos.x + SK_W + 4.0f, contentTop);
            maskPos = ImVec2(wsPos.x + WS_W + 4.0f, contentTop);
        }

        // ── Working Set / ECU Designator Bar ──
        bool isMuller = (cfg.bezelStyle == 0);
        ImU32 wsBg = isMuller ? IM_COL32(20, 23, 29, 255) : IM_COL32(26, 28, 34, 255);
        ImU32 wsBorder = isMuller ? IM_COL32(14, 88, 185, 255) : IM_COL32(58, 63, 74, 255);
        dl->AddRectFilled(wsPos, ImVec2(wsPos.x + wsSize.x, wsPos.y + wsSize.y), wsBg, 4.0f);
        dl->AddRect(wsPos, ImVec2(wsPos.x + wsSize.x, wsPos.y + wsSize.y), wsBorder, 4.0f, 0, isMuller ? 2.0f : 1.5f);

        const auto &allWs = vtServer->get_all_ws();
        float desW = 60.0f * maskScale;
        float desH = 60.0f * maskScale;
        float desX = wsPos.x + (wsSize.x - desW) * 0.5f;

        for (std::size_t wsIdx = 0; wsIdx < allWs.size(); ++wsIdx)
        {
            auto currentWs = allWs[wsIdx];
            if (!currentWs) continue;

            float desY = wsPos.y + 8.0f + static_cast<float>(wsIdx) * (desH + 8.0f * maskScale);
            if (desY + desH > wsPos.y + wsSize.y - (46.0f + 16.0f) * maskScale)
                break;

            ImVec2 desTL(desX, desY);
            ImVec2 desBR(desX + desW, desY + desH);

            bool isActiveWs = (currentWs == ws);

            ImVec2 mousePos = ImGui::GetIO().MousePos;
            bool isHovered = (mousePos.x >= desTL.x && mousePos.x <= desBR.x &&
                              mousePos.y >= desTL.y && mousePos.y <= desBR.y);
            if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                AudioBeeper::get_instance().play_touch_click();
                vtServer->switch_working_set(wsIdx);
            }

            ImU32 bgCol = IM_COL32(255, 255, 255, 255);
            ImU32 borderCol = isActiveWs ? IM_COL32(132, 180, 41, 255) : (isHovered ? IM_COL32(100, 120, 160, 255) : IM_COL32(70, 75, 88, 255));
            float borderThick = isActiveWs ? 2.5f : 1.0f;

            dl->AddRectFilled(desTL, desBR, bgCol, 4.0f);
            dl->AddRect(desTL, desBR, borderCol, 4.0f, 0, borderThick);

            auto wsObj = currentWs->get_working_set_object();
            if (wsObj && wsObj->get_number_children() > 0)
            {
                auto designatorObj = currentWs->get_object_by_id(wsObj->get_child_id(0));
                if (designatorObj)
                {
                    if (designatorObj->get_object_type() == isobus::VirtualTerminalObjectType::ObjectPointer)
                    {
                        auto ptr = std::dynamic_pointer_cast<isobus::ObjectPointer>(designatorObj);
                        if (ptr) designatorObj = currentWs->get_object_by_id(ptr->get_value());
                    }
                    if (designatorObj)
                    {
                        float rawW = static_cast<float>(designatorObj->get_width());
                        float rawH = static_cast<float>(designatorObj->get_height());
                        float fitScale = maskScale;
                        if (rawW > 0.0f && rawH > 0.0f)
                        {
                            float scaleX = (desW - 4.0f) / rawW;
                            float scaleY = (desH - 4.0f) / rawH;
                            fitScale = std::min(scaleX, scaleY);
                        }
                        float objW = rawW * fitScale;
                        float objH = rawH * fitScale;
                        float offX = (desW - objW) * 0.5f;
                        float offY = (desH - objH) * 0.5f;
                        dl->PushClipRect(desTL, desBR, true);
                        render_object(dl, ImVec2(desTL.x + offX, desTL.y + offY), currentWs, designatorObj, 0, 0, 0, fitScale);
                        dl->PopClipRect();
                    }
                }
            }
        }

        // ── Dedicated Alarm Acknowledge (ACK) Button (matching AgIsoVirtualTerminal / agisovt) ──
        if (isWsReady)
        {
            bool alarmActive = vtServer->is_alarm_active();
            static bool ackButtonHeld = false;

            float ackW = 60.0f * maskScale;
            float ackH = 46.0f * maskScale;
            float ackX = wsPos.x + (wsSize.x - ackW) * 0.5f;
            float ackY = wsPos.y + wsSize.y - ackH - 8.0f * maskScale;
            ImVec2 ackTL(ackX, ackY);
            ImVec2 ackBR(ackX + ackW, ackY + ackH);

            ImVec2 mousePos = ImGui::GetIO().MousePos;
            bool isHovered = (mousePos.x >= ackTL.x && mousePos.x <= ackBR.x &&
                              mousePos.y >= ackTL.y && mousePos.y <= ackBR.y);
            bool isMouseDown = isHovered && ImGui::IsMouseDown(ImGuiMouseButton_Left);

            // Keyboard shortcut: ESC triggers alarm ACK immediately (only when an alarm is active!)
            bool escDown = ImGui::IsKeyDown(ImGuiKey_Escape);
            bool currentPressed = (isMouseDown || escDown);

            if (alarmActive)
            {
                if (currentPressed != ackButtonHeld)
                {
                    ackButtonHeld = currentPressed;
                    if (currentPressed)
                    {
                        AudioBeeper::get_instance().play_ack_chime();
                    }
                    vtServer->send_alarm_ack_command(ackButtonHeld);
                }
            }
            else
            {
                ackButtonHeld = false;
            }

            if (alarmActive || isHovered)
            {
                // Pulsing amber/red button when alarm is active
                float pulse = 0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 7.0f);
                ImU32 bgCol = currentPressed ? IM_COL32(180, 45, 25, 255) :
                              (alarmActive ? IM_COL32(static_cast<int>(190 + 55 * pulse), 40, 30, 255)
                                           : IM_COL32(55, 60, 72, 255));
                ImU32 borderCol = currentPressed ? IM_COL32(255, 255, 255, 255) :
                                  (alarmActive ? IM_COL32(255, 220, 50, 255)
                                               : IM_COL32(100, 110, 130, 255));

                dl->AddRectFilled(ackTL, ackBR, bgCol, 5.0f);
                dl->AddRect(ackTL, ackBR, borderCol, 5.0f, 0, currentPressed ? 2.5f : 2.0f);

                std::string ackText = "ACK";
                ImVec2 txtSz = ImGui::CalcTextSize(ackText.c_str());
                float txtX = ackTL.x + (ackW - txtSz.x) * 0.5f;
                float txtY = ackTL.y + (ackH - txtSz.y) * 0.5f - 4.0f * maskScale;
                dl->AddText(ImVec2(txtX, txtY), IM_COL32(255, 255, 255, 255), ackText.c_str());

                std::string hintText = "[ESC]";
                ImVec2 hintSz = ImGui::CalcTextSize(hintText.c_str());
                float hintX = ackTL.x + (ackW - hintSz.x) * 0.5f;
                float hintY = txtY + txtSz.y + 1.0f;
                dl->AddText(ImVec2(hintX, hintY), IM_COL32(255, 230, 160, 230), hintText.c_str());
            }
            else
            {
                // Normal standby ACK button
                ImU32 bgCol = IM_COL32(34, 38, 46, 220);
                ImU32 borderCol = IM_COL32(58, 64, 76, 220);
                dl->AddRectFilled(ackTL, ackBR, bgCol, 5.0f);
                dl->AddRect(ackTL, ackBR, borderCol, 5.0f, 0, 1.0f);

                std::string ackText = "ACK";
                ImVec2 txtSz = ImGui::CalcTextSize(ackText.c_str());
                float txtX = ackTL.x + (ackW - txtSz.x) * 0.5f;
                float txtY = ackTL.y + (ackH - txtSz.y) * 0.5f;
                dl->AddText(ImVec2(txtX, txtY), IM_COL32(110, 120, 140, 200), ackText.c_str());
            }
        }

        

        if (isWsReady && maskId != isobus::NULL_OBJECT_ID && ws->get_object_by_id(maskId) != nullptr)
        {
            render_data_mask(dl, maskPos, maskSize, ws, maskId);
            render_soft_keys(skPos, skSize, ws, skId);
        }
        else
        {
            // ── Standby screen ────────────────────────────────────────────────
            dl->AddRectFilled(maskPos, ImVec2(maskPos.x + DATA_W, maskPos.y + DATA_H),
                              IM_COL32(18, 22, 28, 255), 4.0f);

            std::string title  = "ISOBUS UNIVERSAL TERMINAL";
            std::string sub    = "ISO 11783-6 VT Server  |  AUX-N Ready";
            std::string status = "Waiting for Implement ECU on CAN bus (250 kbit/s)...";
            std::string hint   = "Connect ISOBUS implement ECU via CAN  (e.g. ESP32+MCP2515)";

            std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> transferringWs = nullptr;
            for (auto &w : vtServer->get_all_ws())
            {
                if (w && (w->get_working_set_object() == nullptr ||
                          w->get_object_pool_processing_state() == isobus::VirtualTerminalServerManagedWorkingSet::ObjectPoolProcessingThreadState::Running ||
                          w->get_object_pool_processing_state() == isobus::VirtualTerminalServerManagedWorkingSet::ObjectPoolProcessingThreadState::Success))
                {
                    transferringWs = w;
                    break;
                }
            }

            if (transferringWs)
            {
                if (transferringWs->get_object_pool_processing_state() == isobus::VirtualTerminalServerManagedWorkingSet::ObjectPoolProcessingThreadState::Running ||
                    transferringWs->get_object_pool_processing_state() == isobus::VirtualTerminalServerManagedWorkingSet::ObjectPoolProcessingThreadState::Success)
                {
                    status = "Parsing Object Pool components...";
                    hint   = "Compiling object tree and initializing controls...";
                }
                else
                {
                    status = "Loading Object Pool: " + std::to_string(static_cast<int>(transferringWs->iop_load_percentage())) + "%";
                    hint   = "Receiving ISO 11783-6 IOP components via CAN ETP...";
                }
            }

            auto centerX = [&](const std::string &s) {
                return maskPos.x + (DATA_W - ImGui::CalcTextSize(s.c_str()).x) * 0.5f;
            };
            float cy = maskPos.y + DATA_H * 0.33f;
            dl->AddText(ImVec2(centerX(title),  cy),        IM_COL32(225, 225, 235, 255), title.c_str());
            dl->AddText(ImVec2(centerX(sub),    cy + 26.0f),IM_COL32(130, 155, 190, 255), sub.c_str());

            float pulse = (std::sin(static_cast<float>(ImGui::GetTime()) * 2.8f) + 1.0f) * 0.5f;
            ImU32 dotC  = IM_COL32(
                static_cast<int>(40  + pulse * 170),
                static_cast<int>(185 + pulse * 70),
                static_cast<int>(50  + pulse * 50), 255);
            dl->AddCircleFilled(ImVec2(centerX(status) - 16.0f, cy + 93.0f), 5.5f, dotC);
            dl->AddText(ImVec2(centerX(status), cy + 86.0f), IM_COL32(170, 205, 170, 255), status.c_str());
            dl->AddText(ImVec2(centerX(hint),   cy + 128.0f),IM_COL32(110, 120, 140, 255), hint.c_str());

            if (transferringWs)
            {
                float barW = DATA_W * 0.6f;
                float barH = 16.0f;
                float barX = maskPos.x + (DATA_W - barW) * 0.5f;
                float barY = cy + 155.0f;
                dl->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH), IM_COL32(35, 40, 50, 255), 4.0f);
                dl->AddRect(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH), IM_COL32(80, 90, 110, 255), 4.0f);
                float fillRatio = transferringWs->iop_load_percentage() / 100.0f;
                if (fillRatio < 0.0f) fillRatio = 0.0f;
                if (fillRatio > 1.0f) fillRatio = 1.0f;
                float fillW = (barW - 4.0f) * fillRatio;
                if (fillW > 0.0f)
                {
                    dl->AddRectFilled(ImVec2(barX + 2.0f, barY + 2.0f), ImVec2(barX + 2.0f + fillW, barY + barH - 2.0f), IM_COL32(50, 205, 80, 255), 2.0f);
                }
            }

            render_soft_keys(skPos, skSize, nullptr, isobus::NULL_OBJECT_ID);
        }

        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + topHeaderH + (DATA_H + PADDING * 2.0f) + 10.0f));

        // ── Settings toolbar (below VT display) ───────────────────────────────
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.22f, 0.25f, 0.32f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.38f, 0.52f, 1.0f));

        if (ImGui::Button(" [ Konfiguracja Terminala ] "))
            showMainSettingsDialog = true;
        ImGui::SameLine();
        if (ImGui::Button("  VT Capabilities  "))
            showVTCapabilitiesDialog = true;
        ImGui::SameLine();
        if (ImGui::Button("  Language / Units  "))
            showLanguageDialog = true;
        ImGui::SameLine();
        ImGui::Separator();
        ImGui::SameLine();
        // Scale control — buttons for precise control, no slider (avoids display glitch)
        ImGui::Text("Zoom:");
        ImGui::SameLine();
        if (ImGui::Button("-##zm")) { maskScale = std::max(0.5f, maskScale - 0.25f); save_settings(); }
        ImGui::SameLine();
        ImGui::Text("%.0f%%", maskScale * 100.0f);
        ImGui::SameLine();
        if (ImGui::Button("+##zm")) { maskScale = std::min(3.0f, maskScale + 0.25f); save_settings(); }
        ImGui::SameLine();
        if (ImGui::Button("1:1##zm")) { maskScale = 1.0f; save_settings(); }
        ImGui::SameLine();
        // Show active mask info
        ImGui::TextDisabled("| Maska: %u  SK: %u  WS: %s",
            maskId,
            skId,
            ws ? "OK" : "brak");

        ImGui::PopStyleColor(2);

        render_modals();
        render_settings_dialogs();
        ImGui::End();
    }

    // ─────────────────────────────────────────────────────────────────────────────
    // AgIsoVT Hit-testing and recursive click resolution
    // ─────────────────────────────────────────────────────────────────────────────

    // Helper to get effective width & height of any object
    static void get_effective_object_bounds(std::shared_ptr<isobus::VTObject> obj, int &w, int &h)
    {
        if (!obj) { w = h = 0; return; }
        w = static_cast<int>(obj->get_width());
        h = static_cast<int>(obj->get_height());
        // In ISO 11783-6, InputBoolean has no separate height attribute — it is always a square (height == width)
        if (obj->get_object_type() == isobus::VirtualTerminalObjectType::InputBoolean && h <= 0)
        {
            h = (w > 0) ? w : 24;
        }
    }

    /* static */ bool VTRenderer::object_can_be_clicked(std::shared_ptr<isobus::VTObject> object)
    {
        if (!object) return false;
        switch (object->get_object_type())
        {
            case isobus::VirtualTerminalObjectType::Button:
            case isobus::VirtualTerminalObjectType::InputList:
            case isobus::VirtualTerminalObjectType::InputNumber:
            case isobus::VirtualTerminalObjectType::InputBoolean:
            case isobus::VirtualTerminalObjectType::InputString:
                return true;
            default:
                return false;
        }
    }

    /* static */ std::shared_ptr<isobus::VTObject> VTRenderer::get_clicked_child_recursive(
        std::shared_ptr<isobus::VTObject> object,
        int x, int y,
        std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws)
    {
        if (!object || !ws) return nullptr;

        if (object->get_object_type() == isobus::VirtualTerminalObjectType::ObjectPointer)
        {
            auto ptr = std::dynamic_pointer_cast<isobus::ObjectPointer>(object);
            if (!ptr || ptr->get_value() == isobus::NULL_OBJECT_ID) return nullptr;
            auto child = ws->get_object_by_id(ptr->get_value());
            if (!child) return nullptr;

            int cw = 0, ch = 0;
            get_effective_object_bounds(child, cw, ch);

            if (object_can_be_clicked(child) &&
                x >= 0 && x <= cw &&
                y >= 0 && y <= ch)
            {
                return child;
            }
            else if (!object_can_be_clicked(child))
            {
                return get_clicked_child_recursive(child, x, y, ws);
            }
            return nullptr;
        }

        if (object->get_object_type() == isobus::VirtualTerminalObjectType::Container)
        {
            auto cont = std::dynamic_pointer_cast<isobus::Container>(object);
            if (cont && cont->get_hidden())
                return nullptr;
        }

        // Search children per AgIsoVT logic (order 0 .. N-1)
        for (std::uint16_t i = 0; i < object->get_number_children(); ++i)
        {
            auto child = ws->get_object_by_id(object->get_child_id(i));
            if (!child) continue;

            int cx = object->get_child_x(i);
            int cy = object->get_child_y(i);
            int cw = 0, ch = 0;
            get_effective_object_bounds(child, cw, ch);

            if (object_can_be_clicked(child) &&
                x >= cx && x <= (cx + cw) &&
                y >= cy && y <= (cy + ch))
            {
                return child;
            }
            else if (!object_can_be_clicked(child))
            {
                auto ret = get_clicked_child_recursive(child, x - cx, y - cy, ws);
                if (ret)
                    return ret;
            }
        }

        // Container hit fallback: if clicked within container bounds, and container contains
        // an interactive element (e.g. InputList in a dropdown container frame, or a Button),
        // resolve to that interactive element.
        if (object->get_object_type() == isobus::VirtualTerminalObjectType::Container)
        {
            int objW = 0, objH = 0;
            get_effective_object_bounds(object, objW, objH);
            if (x >= 0 && x <= objW && y >= 0 && y <= objH)
            {
                for (std::uint16_t i = 0; i < object->get_number_children(); ++i)
                {
                    auto child = ws->get_object_by_id(object->get_child_id(i));
                    if (child)
                    {
                        if (child->get_object_type() == isobus::VirtualTerminalObjectType::InputList ||
                            child->get_object_type() == isobus::VirtualTerminalObjectType::InputBoolean ||
                            child->get_object_type() == isobus::VirtualTerminalObjectType::Button)
                        {
                            return child;
                        }
                    }
                }
            }
        }

        

        return nullptr;
    }

    // ─────────────────────────────────────────────────────────────────────────────

    void VTRenderer::render_data_mask(ImDrawList *dl, ImVec2 pos, ImVec2 size,
                                      std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws,
                                      std::uint16_t maskId)
    {
        auto maskObj = ws->get_object_by_id(maskId);
        if (!maskObj) return;

        ImU32 bg = get_imgui_color(ws, maskObj->get_background_color(), IM_COL32(220, 220, 220, 255));
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg, 4.0f);

        // Clip all child rendering to the mask area
        ImVec2 clipMax(pos.x + size.x, pos.y + size.y);
        dl->PushClipRect(pos, clipMax, true);

        for (std::uint16_t i = 0; i < maskObj->get_number_children(); ++i)
        {
            auto cObj = ws->get_object_by_id(maskObj->get_child_id(i));
            if (cObj)
            {
                render_object(dl, pos, ws, cObj,
                              maskObj->get_child_x(i),
                              maskObj->get_child_y(i),
                              maskId,
                              maskScale);
            }
        }

        dl->PopClipRect();

        // ── DataMask Centralized Mouse Interaction (AgIsoVT architecture) ──
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton("##DataMaskHitArea", size);
        ImVec2 mousePos = ImGui::GetIO().MousePos;
        bool inMaskBounds = (mousePos.x >= pos.x && mousePos.x <= pos.x + size.x &&
                             mousePos.y >= pos.y && mousePos.y <= pos.y + size.y);
        bool isHovered = ImGui::IsItemHovered() || inMaskBounds;
        bool isClicked = ImGui::IsItemClicked(0) || (ImGui::IsMouseClicked(0) && inMaskBounds);
        bool isReleased = ImGui::IsItemDeactivated() || (ImGui::IsMouseReleased(0) && inMaskBounds);

        if (maskScale > 0.001f)
        {
            int localX = static_cast<int>((mousePos.x - pos.x) / maskScale);
            int localY = static_cast<int>((mousePos.y - pos.y) / maskScale);

            // Mouse Down: Button press
            if (isClicked)
            {
                auto clickedObj = get_clicked_child_recursive(maskObj, localX, localY, ws);
                if (clickedObj && clickedObj->get_object_type() == isobus::VirtualTerminalObjectType::Button)
                {
                    auto btn = std::dynamic_pointer_cast<isobus::Button>(clickedObj);
                    if (btn && !btn->get_option(isobus::Button::Options::Disabled))
                    {
                        activePressedButton = clickedObj;
                        activePressedButtonMaskId = maskId;
                        std::uint8_t kc = btn->get_key_code();
                        vtServer->trigger_macro(clickedObj, isobus::EventID::OnKeyPress, isobus::VirtualTerminalObjectType::Button);
                        vtServer->activate_button(clickedObj->get_id(), maskId, kc, true);
                        AudioBeeper::get_instance().play_touch_click();
                    }
                }
            }
        }

        

        // Mouse Up: Button release & input fields selection
        if (isReleased || ImGui::IsMouseReleased(0))
        {
            // 1. Release active button if held
            if (activePressedButton)
            {
                auto btn = std::dynamic_pointer_cast<isobus::Button>(activePressedButton);
                std::uint8_t kc = btn ? btn->get_key_code() : 1;
                vtServer->activate_button(activePressedButton->get_id(), activePressedButtonMaskId, kc, false);
                vtServer->trigger_macro(activePressedButton, isobus::EventID::OnKeyRelease, isobus::VirtualTerminalObjectType::Button);
                activePressedButton = nullptr;
            }

            // 2. Click events for other interactive objects
            if ((isHovered || inMaskBounds) && maskScale > 0.001f)
            {
                int localX = static_cast<int>((mousePos.x - pos.x) / maskScale);
                int localY = static_cast<int>((mousePos.y - pos.y) / maskScale);
                auto releasedObj = get_clicked_child_recursive(maskObj, localX, localY, ws);
                if (releasedObj)
                {
                    switch (releasedObj->get_object_type())
                    {
                        case isobus::VirtualTerminalObjectType::InputBoolean:
                        {
                            auto inBool = std::dynamic_pointer_cast<isobus::InputBoolean>(releasedObj);
                            if (inBool)
                            {
                                vtServer->trigger_macro(inBool, isobus::EventID::OnEntryOfAValue, isobus::VirtualTerminalObjectType::InputBoolean);
                                vtServer->trigger_macro(inBool, isobus::EventID::OnEntryOfANewValue, isobus::VirtualTerminalObjectType::InputBoolean);

                                bool currentVal = (inBool->get_value() != 0);
                                auto varId = inBool->get_variable_reference();
                                if (varId != isobus::NULL_OBJECT_ID)
                                {
                                    auto numVar = std::dynamic_pointer_cast<isobus::NumberVariable>(ws->get_object_by_id(varId));
                                    if (numVar)
                                    {
                                        currentVal = (numVar->get_value() != 0);
                                        std::uint32_t newVal = currentVal ? 0 : 1;
                                        numVar->set_value(newVal);
                                        vtServer->change_numeric_value(numVar->get_id(), newVal);
                                        vtServer->trigger_macro(numVar, isobus::EventID::OnChangeValue, isobus::VirtualTerminalObjectType::NumberVariable);
                                    }
                                }
                                else
                                {
                                    std::uint8_t newVal = currentVal ? 0 : 1;
                                    inBool->set_value(newVal);
                                    vtServer->change_numeric_value(inBool->get_id(), newVal);
                                    vtServer->trigger_macro(inBool, isobus::EventID::OnChangeValue, isobus::VirtualTerminalObjectType::InputBoolean);
                                }
                            }
                            break;
                        }

                        case isobus::VirtualTerminalObjectType::InputList:
                        {
                            auto inList = std::dynamic_pointer_cast<isobus::InputList>(releasedObj);
                            if (inList)
                            {
                                editingListObjectId = inList->get_id();
                                showInputListPopup = true;
                            }
                            break;
                        }

                        case isobus::VirtualTerminalObjectType::InputNumber:
                        {
                            auto inNum = std::dynamic_pointer_cast<isobus::InputNumber>(releasedObj);
                            if (inNum && inNum->get_option2(isobus::InputNumber::Options2::Enabled))
                            {
                                editingNumberObjectId = inNum->get_id();
                                uint32_t rawVal = inNum->get_value();
                                auto varId = inNum->get_variable_reference();
                                if (varId != isobus::NULL_OBJECT_ID)
                                {
                                    auto varObj = std::dynamic_pointer_cast<isobus::NumberVariable>(ws->get_object_by_id(varId));
                                    if (varObj) rawVal = varObj->get_value();
                                }
                                editingNumberValue = static_cast<int>(rawVal);
                                showNumberKeypad = true;
                            }
                            break;
                        }

                        case isobus::VirtualTerminalObjectType::InputString:
                        {
                            auto inStr = std::dynamic_pointer_cast<isobus::InputString>(releasedObj);
                            if (inStr && inStr->get_enabled())
                            {
                                editingStringObjectId = inStr->get_id();
                                std::string text = inStr->displayed_value(ws->get_object_tree());
                                strncpy_s(editingStringBuffer, text.c_str(), sizeof(editingStringBuffer) - 1);
                                showStringEditor = true;
                            }
                            break;
                        }

                        default:
                            break;
                    }
                }
            }
        }

        
    }

    // ─────────────────────────────────────────────────────────────────────────────

    void VTRenderer::render_object(ImDrawList *dl, ImVec2 parentPos,
                                   std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws,
                                   std::shared_ptr<isobus::VTObject> obj,
                                   std::int16_t relX, std::int16_t relY,
                                   std::uint16_t parentId,
                                   float scale)
    {
        if (!obj) return;

        ImVec2 objPos(parentPos.x + static_cast<float>(relX) * scale,
                      parentPos.y + static_cast<float>(relY) * scale);
        float w = static_cast<float>(obj->get_width())  * scale;
        float h = static_cast<float>(obj->get_height()) * scale;

        const auto &tree = ws->get_object_tree();

        switch (obj->get_object_type())
        {
            // ── Container ────────────────────────────────────────────────────
            case isobus::VirtualTerminalObjectType::Container:
            {
                auto cont = std::dynamic_pointer_cast<isobus::Container>(obj);
                if (cont && cont->get_hidden())
                    break; // Skip hidden containers per ISO 11783-6 and AgIsoVT

                for (std::uint16_t i = 0; i < obj->get_number_children(); ++i)
                {
                    auto cObj = ws->get_object_by_id(obj->get_child_id(i));
                    if (cObj)
                        render_object(dl, objPos, ws, cObj,
                                      obj->get_child_x(i), obj->get_child_y(i),
                                      obj->get_id(), scale);
                }
                break;
            }

            // ── Button ───────────────────────────────────────────────────────
            case isobus::VirtualTerminalObjectType::Button:
            {
                auto btn = std::dynamic_pointer_cast<isobus::Button>(obj);
                if (w <= 0.0f) w = 80.0f * scale;
                if (h <= 0.0f) h = 30.0f * scale;

                ImVec2 bMax(objPos.x + w, objPos.y + h);
                bool isPressed = (activePressedButton && activePressedButton->get_id() == obj->get_id());

                bool transparent = btn && btn->get_option(isobus::Button::Options::TransparentBackground);
                bool noBorder    = btn && (btn->get_option(isobus::Button::Options::SuppressBorder) ||
                                            btn->get_option(isobus::Button::Options::NoBorder));

                if (!transparent)
                {
                    ImU32 bg = get_imgui_color(ws, obj->get_background_color(), IM_COL32(200, 205, 215, 255));
                    if (isPressed) bg = IM_COL32(130, 150, 185, 255);
                    dl->AddRectFilled(objPos, bMax, bg, 4.0f);
                }
                if (!noBorder)
                {
                    ImU32 borderC = btn ? get_imgui_color(ws, btn->get_border_colour(), IM_COL32(60, 65, 78, 255))
                                       : IM_COL32(60, 65, 78, 255);
                    dl->AddRect(objPos, bMax, borderC, 4.0f, 0, 1.5f);
                }

                for (std::uint16_t i = 0; i < obj->get_number_children(); ++i)
                {
                    auto cObj = ws->get_object_by_id(obj->get_child_id(i));
                    if (cObj)
                        render_object(dl, objPos, ws, cObj,
                                      obj->get_child_x(i), obj->get_child_y(i),
                                      obj->get_id(), scale);
                }
                break;
            }

            // ── OutputString ─────────────────────────────────────────────────
            case isobus::VirtualTerminalObjectType::OutputString:
            {
                auto outStr = std::dynamic_pointer_cast<isobus::OutputString>(obj);
                if (!outStr) break;

                bool transparent = outStr->get_option(isobus::StringVTObject::Options::Transparent);
                if (!transparent)
                {
                    ImU32 bg = get_imgui_color(ws, obj->get_background_color(), IM_COL32(255, 255, 255, 255));
                    if (w > 0.0f && h > 0.0f && obj->get_background_color() != 254 && (bg >> 24) > 0)
                        dl->AddRectFilled(objPos, ImVec2(objPos.x + w, objPos.y + h), bg);
                }

                ImU32 textCol = get_imgui_color(ws, 0, IM_COL32(0, 0, 0, 255));
                float fontH = 14.0f;
                resolve_font(ws, outStr->get_font_attributes(), textCol, fontH);

                std::string text = outStr->displayed_value(tree);
                draw_text_justified(dl, text, objPos, w > 0 ? w : (200.0f * scale), h > 0 ? h : (20.0f * scale),
                                    textCol, fontH, scale,
                                    outStr->get_horizontal_justification(),
                                    outStr->get_vertical_justification());
                break;
            }

            // ── OutputNumber ─────────────────────────────────────────────────
            case isobus::VirtualTerminalObjectType::OutputNumber:
            {
                auto outNum = std::dynamic_pointer_cast<isobus::OutputNumber>(obj);
                if (!outNum) break;

                bool transparent = outNum->get_option(isobus::NumberVTObject::Options::Transparent);
                if (!transparent)
                {
                    ImU32 bg = get_imgui_color(ws, obj->get_background_color(), IM_COL32(255, 255, 255, 255));
                    if (w > 0.0f && h > 0.0f && obj->get_background_color() != 254 && (bg >> 24) > 0)
                        dl->AddRectFilled(objPos, ImVec2(objPos.x + w, objPos.y + h), bg);
                }

                ImU32 textCol = get_imgui_color(ws, 0, IM_COL32(0, 0, 0, 255));
                float fontH = 14.0f;
                resolve_font(ws, outNum->get_font_attributes(), textCol, fontH);

                uint32_t rawVal = outNum->get_value();
                auto varId = outNum->get_variable_reference();
                if (varId != isobus::NULL_OBJECT_ID && ws)
                {
                    auto varObj = std::dynamic_pointer_cast<isobus::NumberVariable>(ws->get_object_by_id(varId));
                    if (varObj)
                        rawVal = varObj->get_value();
                }

                double val = (static_cast<double>(static_cast<int64_t>(rawVal))
                              + static_cast<double>(outNum->get_offset()))
                             * static_cast<double>(outNum->get_scale());
                std::ostringstream oss;
                oss << std::fixed << std::setprecision(outNum->get_number_of_decimals()) << val;
                std::string text = oss.str();

                // DisplayZeroAsBlank
                if (outNum->get_option(isobus::NumberVTObject::Options::DisplayZeroAsBlank) && val == 0.0)
                    text = "";

                draw_text_justified(dl, text, objPos, w > 0 ? w : (100.0f * scale), h > 0 ? h : (20.0f * scale),
                                    textCol, fontH, scale,
                                    outNum->get_horizontal_justification(),
                                    outNum->get_vertical_justification());
                break;
            }

            // ── InputString ──────────────────────────────────────────────────
            case isobus::VirtualTerminalObjectType::InputString:
            {
                auto inStr = std::dynamic_pointer_cast<isobus::InputString>(obj);
                if (w <= 0.0f) w = 120.0f * scale;
                if (h <= 0.0f) h = 22.0f * scale;
                ImVec2 bMax(objPos.x + w, objPos.y + h);

                ImU32 bg = get_imgui_color(ws, obj->get_background_color(), IM_COL32(255, 255, 255, 255));
                dl->AddRectFilled(objPos, bMax, bg, 2.0f);
                dl->AddRect(objPos, bMax, IM_COL32(80, 80, 100, 255), 2.0f, 0, 1.5f);

                if (inStr)
                {
                    ImU32 textCol = get_imgui_color(ws, 0, IM_COL32(0, 0, 0, 255));
                    float fontH = 14.0f;
                    resolve_font(ws, inStr->get_font_attributes(), textCol, fontH);
                    std::string text = inStr->displayed_value(tree);
                    draw_text_justified(dl, text, ImVec2(objPos.x + 3.0f, objPos.y), w - 6.0f, h,
                                        textCol, fontH, scale,
                                        inStr->get_horizontal_justification(),
                                        inStr->get_vertical_justification());

                    if (ImGui::IsMouseHoveringRect(objPos, bMax) && ImGui::IsMouseClicked(0))
                    {
                        editingStringObjectId = obj->get_id();
                        strncpy_s(editingStringBuffer, text.c_str(), sizeof(editingStringBuffer) - 1);
                        showStringEditor = true;
                    }
                }
                break;
            }

            // ── InputNumber ──────────────────────────────────────────────────
            case isobus::VirtualTerminalObjectType::InputNumber:
            {
                auto inNum = std::dynamic_pointer_cast<isobus::InputNumber>(obj);
                if (w <= 0.0f) w = 100.0f * scale;
                if (h <= 0.0f) h = 22.0f * scale;
                ImVec2 bMax(objPos.x + w, objPos.y + h);

                ImU32 bg = get_imgui_color(ws, obj->get_background_color(), IM_COL32(255, 255, 255, 255));
                dl->AddRectFilled(objPos, bMax, bg, 2.0f);
                dl->AddRect(objPos, bMax, IM_COL32(80, 80, 100, 255), 2.0f, 0, 1.5f);

                if (inNum)
                {
                    ImU32 textCol = get_imgui_color(ws, 0, IM_COL32(0, 0, 0, 255));
                    float fontH = 14.0f;
                    resolve_font(ws, inNum->get_font_attributes(), textCol, fontH);

                    uint32_t rawVal = inNum->get_value();
                    auto varId = inNum->get_variable_reference();
                    if (varId != isobus::NULL_OBJECT_ID && ws)
                    {
                        auto varObj = std::dynamic_pointer_cast<isobus::NumberVariable>(ws->get_object_by_id(varId));
                        if (varObj)
                            rawVal = varObj->get_value();
                    }

                    double val = (static_cast<double>(static_cast<int64_t>(rawVal))
                                  + static_cast<double>(inNum->get_offset()))
                                 * static_cast<double>(inNum->get_scale());
                    std::ostringstream oss;
                    oss << std::fixed << std::setprecision(inNum->get_number_of_decimals()) << val;
                    std::string text = oss.str();

                    draw_text_justified(dl, text, ImVec2(objPos.x + 3.0f, objPos.y), w - 6.0f, h,
                                        textCol, fontH, scale,
                                        inNum->get_horizontal_justification(),
                                        inNum->get_vertical_justification());

                    if (ImGui::IsMouseHoveringRect(objPos, bMax) && ImGui::IsMouseClicked(0))
                    {
                        editingNumberObjectId = obj->get_id();
                        editingNumberValue    = static_cast<int>(rawVal);
                        showNumberKeypad      = true;
                    }
                }
                break;
            }

            // ── InputBoolean ─────────────────────────────────────────────────
            case isobus::VirtualTerminalObjectType::InputBoolean:
            {
                auto inBool = std::dynamic_pointer_cast<isobus::InputBoolean>(obj);
                if (w <= 0.0f) w = 24.0f * scale;
                if (h <= 0.0f) h = 24.0f * scale;
                ImVec2 bMax(objPos.x + w, objPos.y + h);

                bool val = inBool && (inBool->get_value() != 0);
                if (inBool && inBool->get_variable_reference() != isobus::NULL_OBJECT_ID)
                {
                    auto varObj = std::dynamic_pointer_cast<isobus::NumberVariable>(ws->get_object_by_id(inBool->get_variable_reference()));
                    if (varObj) val = (varObj->get_value() != 0);
                }

                ImU32 bg = get_imgui_color(ws, inBool ? inBool->get_background_color() : 0, IM_COL32(245, 245, 250, 255));
                dl->AddRectFilled(objPos, bMax, bg, 3.0f);
                dl->AddRect(objPos, bMax, IM_COL32(70, 75, 90, 255), 3.0f, 0, 1.5f);

                if (val)
                {
                    // Green checkmark tick
                    ImVec2 p1(objPos.x + w * 0.22f, objPos.y + h * 0.52f);
                    ImVec2 p2(objPos.x + w * 0.44f, objPos.y + h * 0.78f);
                    ImVec2 p3(objPos.x + w * 0.82f, objPos.y + h * 0.22f);
                    dl->AddLine(p1, p2, IM_COL32(25, 160, 45, 255), 2.5f * scale);
                    dl->AddLine(p2, p3, IM_COL32(25, 160, 45, 255), 2.5f * scale);
                }
                break;
            }

            // ── InputList ─────────────────────────────────────────────────────
            case isobus::VirtualTerminalObjectType::InputList:
            {
                auto inList = std::dynamic_pointer_cast<isobus::InputList>(obj);
                if (!inList) break;
                if (w <= 0.0f) w = 140.0f * scale;
                if (h <= 0.0f) h = 24.0f * scale;
                ImVec2 bMax(objPos.x + w, objPos.y + h);

                std::uint32_t selectedIndex = inList->get_value();
                if (inList->get_variable_reference() != isobus::NULL_OBJECT_ID)
                {
                    auto varObj = std::dynamic_pointer_cast<isobus::NumberVariable>(ws->get_object_by_id(inList->get_variable_reference()));
                    if (varObj) selectedIndex = varObj->get_value();
                }

                ImU32 bg = get_imgui_color(ws, inList->get_background_color(), IM_COL32(255, 255, 255, 255));
                dl->AddRectFilled(objPos, bMax, bg, 2.0f);
                dl->AddRect(objPos, bMax, IM_COL32(60, 65, 80, 255), 2.0f, 0, 1.5f);

                // Dropdown arrow button on right side
                float arrowW = std::min(24.0f * scale, w * 0.25f);
                ImVec2 arrowTL(bMax.x - arrowW, objPos.y);
                dl->AddRectFilled(arrowTL, bMax, IM_COL32(230, 232, 238, 255), 0.0f);
                dl->AddLine(arrowTL, ImVec2(arrowTL.x, bMax.y), IM_COL32(160, 165, 180, 255), 1.0f);

                // Arrow triangle
                float ax = arrowTL.x + arrowW * 0.5f;
                float ay = objPos.y + h * 0.5f;
                float as = 4.0f * scale;
                dl->AddTriangleFilled(ImVec2(ax - as, ay - as * 0.6f),
                                      ImVec2(ax + as, ay - as * 0.6f),
                                      ImVec2(ax, ay + as * 0.8f),
                                      IM_COL32(60, 65, 80, 255));

                // Render selected item inside box (ISO 11783-6: 1-based, 1 = first child, 0 = none)
                std::uint16_t childIdx = 0;
                bool hasChild = false;
                if (selectedIndex >= 1 && selectedIndex <= inList->get_number_children())
                {
                    childIdx = static_cast<std::uint16_t>(selectedIndex - 1);
                    hasChild = true;
                }
                else if (selectedIndex == 0 && inList->get_number_children() > 0)
                {
                    childIdx = 0;
                    hasChild = true;
                }

                if (hasChild)
                {
                    auto childObj = ws->get_object_by_id(inList->get_child_id(childIdx));
                    if (childObj)
                    {
                        if (childObj->get_object_type() == isobus::VirtualTerminalObjectType::OutputString)
                        {
                            auto outStr = std::dynamic_pointer_cast<isobus::OutputString>(childObj);
                            ImU32 textCol = IM_COL32(0, 0, 0, 255);
                            float fontH = 14.0f;
                            resolve_font(ws, outStr->get_font_attributes(), textCol, fontH);
                            std::string text = outStr->displayed_value(tree);
                            draw_text_justified(dl, text, ImVec2(objPos.x + 4.0f, objPos.y), w - arrowW - 6.0f, h,
                                                textCol, fontH, scale,
                                                isobus::TextualVTObject::HorizontalJustification::PositionLeft,
                                                isobus::TextualVTObject::VerticalJustification::PositionMiddle);
                        }
                        else
                        {
                            render_object(dl, objPos, ws, childObj, 2, 2, obj->get_id(), scale);
                        }
                    }
                }
                break;
            }

            // ── OutputRectangle ──────────────────────────────────────────────
            case isobus::VirtualTerminalObjectType::OutputRectangle:
            {
                auto rect = std::dynamic_pointer_cast<isobus::OutputRectangle>(obj);
                if (w <= 0.0f || h <= 0.0f) break;

                ImU32 fillCol  = IM_COL32(255, 255, 255, 255);
                ImU32 lineCol  = IM_COL32(0, 0, 0, 255);
                float lineW    = 1.0f;
                bool  hasFill  = false;
                bool  hasLine  = true;

                if (rect)
                {
                    hasFill = resolve_fill(ws, rect->get_fill_attributes(), fillCol);
                    resolve_line(ws, rect->get_line_attributes(), lineCol, lineW);

                    // LineSuppression: 4 independent sides
                    std::uint8_t sup = rect->get_line_suppression_bitfield();

                    if (hasFill)
                        dl->AddRectFilled(objPos, ImVec2(objPos.x + w, objPos.y + h), fillCol);

                    if (hasLine && lineW > 0.0f)
                    {
                        float lh = lineW * 0.5f;
                        // Top
                        if (!(sup & (1 << static_cast<int>(isobus::OutputRectangle::LineSuppressionOption::SuppressTopLine))))
                            dl->AddLine(objPos, ImVec2(objPos.x + w, objPos.y), lineCol, lineW);
                        // Right
                        if (!(sup & (1 << static_cast<int>(isobus::OutputRectangle::LineSuppressionOption::SuppressRightSideLine))))
                            dl->AddLine(ImVec2(objPos.x + w, objPos.y), ImVec2(objPos.x + w, objPos.y + h), lineCol, lineW);
                        // Bottom
                        if (!(sup & (1 << static_cast<int>(isobus::OutputRectangle::LineSuppressionOption::SuppressBottomLine))))
                            dl->AddLine(ImVec2(objPos.x, objPos.y + h), ImVec2(objPos.x + w, objPos.y + h), lineCol, lineW);
                        // Left
                        if (!(sup & (1 << static_cast<int>(isobus::OutputRectangle::LineSuppressionOption::SuppressLeftSideLine))))
                            dl->AddLine(objPos, ImVec2(objPos.x, objPos.y + h), lineCol, lineW);
                        (void)lh;
                    }
                }
                break;
            }

            // ── OutputLine ───────────────────────────────────────────────────
            case isobus::VirtualTerminalObjectType::OutputLine:
            {
                auto lineObj = std::dynamic_pointer_cast<isobus::OutputLine>(obj);
                ImU32 lineCol = get_imgui_color(ws, 0, IM_COL32(0, 0, 0, 255));
                float lineW   = 1.0f;
                if (lineObj)
                    resolve_line(ws, lineObj->get_line_attributes(), lineCol, lineW);

                // Direction: 0 = TL→BR, 1 = BL→TR
                bool bottomLeft = lineObj &&
                    (lineObj->get_line_direction() == isobus::OutputLine::LineDirection::BottomLeftToTopRight);

                ImVec2 p1 = bottomLeft ? ImVec2(objPos.x,     objPos.y + h) : objPos;
                ImVec2 p2 = bottomLeft ? ImVec2(objPos.x + w, objPos.y)     : ImVec2(objPos.x + w, objPos.y + h);
                dl->AddLine(p1, p2, lineCol, lineW);
                break;
            }

            // ── OutputEllipse ────────────────────────────────────────────────
            case isobus::VirtualTerminalObjectType::OutputEllipse:
            {
                auto ell = std::dynamic_pointer_cast<isobus::OutputEllipse>(obj);
                if (!ell || w <= 0.0f || h <= 0.0f) break;

                ImVec2 center(objPos.x + w * 0.5f, objPos.y + h * 0.5f);
                float rx = w * 0.5f;
                float ry = h * 0.5f;

                ImU32 fillCol = IM_COL32(200, 200, 200, 255);
                ImU32 lineCol = IM_COL32(0, 0, 0, 255);
                float lineW   = 1.0f;
                bool hasFill  = resolve_fill(ws, ell->get_fill_attributes(), fillCol);
                resolve_line(ws, ell->get_line_attributes(), lineCol, lineW);

                const int SEG = 64;
                auto ellipseType = ell->get_ellipse_type();

                float startDeg = static_cast<float>(ell->get_start_angle()) * 2.0f;
                float endDeg   = static_cast<float>(ell->get_end_angle())   * 2.0f;

                if (ellipseType == isobus::OutputEllipse::EllipseType::Closed ||
                    ell->get_start_angle() == ell->get_end_angle())
                {
                    if (hasFill)
                        dl->AddEllipseFilled(center, ImVec2(rx, ry), fillCol, 0.0f, SEG);
                    dl->AddEllipse(center, ImVec2(rx, ry), lineCol, 0.0f, SEG, lineW);
                }
                else
                {
                    // Partial ellipse (counter-clockwise from start to end)
                    if (endDeg <= startDeg) endDeg += 360.0f;
                    float spanDeg = endDeg - startDeg;
                    std::vector<ImVec2> pts;
                    for (int i = 0; i <= SEG; ++i)
                    {
                        float aDeg = startDeg + spanDeg * static_cast<float>(i) / SEG;
                        float aRad = aDeg * static_cast<float>(M_PI) / 180.0f;
                        pts.push_back(ImVec2(center.x + rx * std::cos(aRad), center.y - ry * std::sin(aRad)));
                    }
                    if (ellipseType == isobus::OutputEllipse::EllipseType::ClosedEllipseSection)
                        pts.push_back(center);

                    if (hasFill && pts.size() >= 3)
                        dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), fillCol);

                    for (int i = 0; i + 1 < static_cast<int>(pts.size()); ++i)
                        dl->AddLine(pts[i], pts[i + 1], lineCol, lineW);

                    if (ellipseType == isobus::OutputEllipse::EllipseType::ClosedEllipseSection ||
                        ellipseType == isobus::OutputEllipse::EllipseType::ClosedEllipseSegment)
                    {
                        // Close the arc
                        if (!pts.empty())
                            dl->AddLine(pts.back(), pts.front(), lineCol, lineW);
                    }
                }
                break;
            }

            // ── OutputPolygon ────────────────────────────────────────────────
            case isobus::VirtualTerminalObjectType::OutputPolygon:
            {
                auto poly = std::dynamic_pointer_cast<isobus::OutputPolygon>(obj);
                if (!poly || poly->get_number_of_points() < 2) break;

                ImU32 fillCol = IM_COL32(200, 200, 200, 255);
                ImU32 lineCol = IM_COL32(0, 0, 0, 255);
                float lineW   = 1.0f;
                bool hasFill  = resolve_fill(ws, poly->get_fill_attributes(), fillCol);
                resolve_line(ws, poly->get_line_attributes(), lineCol, lineW);

                std::uint8_t nPts = poly->get_number_of_points();
                std::vector<ImVec2> pts;
                pts.reserve(nPts);
                for (std::uint8_t i = 0; i < nPts; ++i)
                {
                    auto pt = poly->get_point(i);
                    pts.push_back(ImVec2(objPos.x + pt.xValue, objPos.y + pt.yValue));
                }

                bool open = poly->get_type() == isobus::OutputPolygon::PolygonType::Open;

                if (hasFill && !open && pts.size() >= 3)
                    dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), fillCol);

                for (int i = 0; i + 1 < static_cast<int>(pts.size()); ++i)
                    dl->AddLine(pts[i], pts[i + 1], lineCol, lineW);
                if (!open && pts.size() >= 2)
                    dl->AddLine(pts.back(), pts.front(), lineCol, lineW);
                break;
            }

            // ── OutputLinearBarGraph ─────────────────────────────────────────
            case isobus::VirtualTerminalObjectType::OutputLinearBarGraph:
            {
                auto bar = std::dynamic_pointer_cast<isobus::OutputLinearBarGraph>(obj);
                if (!bar || w <= 0.0f || h <= 0.0f) break;
                draw_linear_bar_graph(dl, ws, bar, objPos, w, h);
                break;
            }

            // ── OutputArchedBarGraph ─────────────────────────────────────────
            case isobus::VirtualTerminalObjectType::OutputArchedBarGraph:
            {
                auto arc = std::dynamic_pointer_cast<isobus::OutputArchedBarGraph>(obj);
                if (!arc) break;
                if (w <= 0.0f) w = static_cast<float>(arc->get_width()) * scale;
                if (h <= 0.0f) h = w;
                if (w <= 0.0f || h <= 0.0f) break;

                float cx = objPos.x + w * 0.5f;
                float cy = objPos.y + h * 0.5f;
                float r  = std::min(w, h) * 0.5f - 2.0f;
                ImU32 arcCol = get_imgui_color(ws, arc->get_colour(), IM_COL32(60, 160, 60, 255));

                float startVtAngleDeg = static_cast<float>(arc->get_start_angle()) * 2.0f;
                float endVtAngleDeg   = static_cast<float>(arc->get_end_angle())   * 2.0f;
                if (endVtAngleDeg < startVtAngleDeg) endVtAngleDeg += 360.0f;

                auto toScreenPos = [cx, cy](float angleDeg, float radius) -> ImVec2 {
                    float rad = angleDeg * static_cast<float>(M_PI) / 180.0f;
                    return ImVec2(cx + radius * std::cos(rad), cy - radius * std::sin(rad));
                };

                std::uint32_t valU = arc->get_value();
                if (arc->get_variable_reference() != isobus::NULL_OBJECT_ID && ws)
                {
                    auto varObj = ws->get_object_by_id(arc->get_variable_reference());
                    if (varObj && varObj->get_object_type() == isobus::VirtualTerminalObjectType::NumberVariable)
                    {
                        auto numVar = std::dynamic_pointer_cast<isobus::NumberVariable>(varObj);
                        if (numVar) valU = numVar->get_value();
                    }
                }

                float minV = static_cast<float>(arc->get_min_value());
                float maxV = static_cast<float>(arc->get_max_value());
                float val  = static_cast<float>(valU);
                float frac = (maxV > minV) ? (val - minV) / (maxV - minV) : 0.0f;
                frac = std::max(0.0f, std::min(1.0f, frac));

                float bgw = static_cast<float>(arc->get_bar_graph_width()) * scale;
                if (bgw < 2.0f) bgw = 6.0f;

                // Full background arc
                const int SEG = 48;
                ImVec2 prevPt = toScreenPos(endVtAngleDeg, r);
                for (int i = 1; i <= SEG; ++i)
                {
                    float t = static_cast<float>(i) / static_cast<float>(SEG);
                    float angle_t = endVtAngleDeg + t * (startVtAngleDeg - endVtAngleDeg);
                    ImVec2 pt = toScreenPos(angle_t, r);
                    dl->AddLine(prevPt, pt, IM_COL32(180, 180, 180, 255), bgw);
                    prevPt = pt;
                }
                // Value arc
                prevPt = toScreenPos(endVtAngleDeg, r);
                int valSeg = static_cast<int>(std::round(SEG * frac));
                for (int i = 1; i <= valSeg; ++i)
                {
                    float t = static_cast<float>(i) / static_cast<float>(SEG);
                    float angle_t = endVtAngleDeg + t * (startVtAngleDeg - endVtAngleDeg);
                    ImVec2 pt = toScreenPos(angle_t, r);
                    dl->AddLine(prevPt, pt, arcCol, bgw);
                    prevPt = pt;
                }
                break;
            }

            // ── OutputMeter ──────────────────────────────────────────────────
            case isobus::VirtualTerminalObjectType::OutputMeter:
            {
                auto meter = std::dynamic_pointer_cast<isobus::OutputMeter>(obj);
                if (!meter) break;
                if (w <= 0.0f) w = static_cast<float>(meter->get_width()) * scale;
                if (h <= 0.0f) h = w;
                if (w <= 0.0f || h <= 0.0f) break;
                draw_meter(dl, ws, meter, objPos, w, h);
                break;
            }

            // ── PictureGraphic ────────────────────────────────────────────────
            case isobus::VirtualTerminalObjectType::PictureGraphic:
            {
                auto pic = std::dynamic_pointer_cast<isobus::PictureGraphic>(obj);
                if (!pic) break;

                // Decode and render the bitmap with target display dimensions
                draw_picture_graphic(dl, ws, pic, objPos, w, h);
                break;
            }

            // ── WorkingSet ────────────────────────────────────────────────────
            case isobus::VirtualTerminalObjectType::WorkingSet:
            {
                // WorkingSet rendered like a container from the mask
                for (std::uint16_t i = 0; i < obj->get_number_children(); ++i)
                {
                    auto cObj = ws->get_object_by_id(obj->get_child_id(i));
                    if (cObj)
                        render_object(dl, objPos, ws, cObj,
                                      obj->get_child_x(i), obj->get_child_y(i),
                                      obj->get_id(), scale);
                }
                break;
            }

            // ── ObjectPointer ────────────────────────────────────────────────
            case isobus::VirtualTerminalObjectType::ObjectPointer:
            {
                auto ptr = std::dynamic_pointer_cast<isobus::ObjectPointer>(obj);
                if (ptr && ptr->get_value() != isobus::NULL_OBJECT_ID)
                {
                    auto target = ws->get_object_by_id(ptr->get_value());
                    if (target)
                    {
                        render_object(dl, objPos, ws, target, 0, 0, obj->get_id(), scale);
                    }
                }
                break;
            }

            default:
                break;
        }
    }

    // ─────────────────────────────────────────────────────────────────────────────

    void VTRenderer::render_soft_keys(ImVec2 barPos, ImVec2 barSize,
                                      std::shared_ptr<isobus::VirtualTerminalServerManagedWorkingSet> ws,
                                      std::uint16_t softKeyMaskId)
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();

        const auto &cfg = TerminalSettings::get_instance().get_config();
        bool isMuller = (cfg.bezelStyle == 0);

        ImU32 barBg = isMuller ? IM_COL32(20, 23, 29, 255) : IM_COL32(26, 28, 34, 255);
        ImU32 barBorder = isMuller ? IM_COL32(14, 88, 185, 255) : IM_COL32(58, 63, 74, 255);
        dl->AddRectFilled(barPos, ImVec2(barPos.x + barSize.x, barPos.y + barSize.y), barBg, 4.0f);
        dl->AddRect(barPos, ImVec2(barPos.x + barSize.x, barPos.y + barSize.y), barBorder, 4.0f, 0, isMuller ? 2.0f : 1.5f);

        // Use configured SK row count
        int numKeys = static_cast<int>(vtServer->get_configured_sk_rows());
        if (numKeys < 1) numKeys = 1;
        if (numKeys > 12) numKeys = 12;

        float keySpacing = 7.0f;
        float keyHeight  = (barSize.y - keySpacing * (numKeys + 1)) / static_cast<float>(numKeys);
        float keyWidth   = barSize.x - 12.0f;

        // Resolve the SoftKeyMask object
        auto skMaskObj = (ws && softKeyMaskId != isobus::NULL_OBJECT_ID)
                             ? ws->get_object_by_id(softKeyMaskId)
                             : nullptr;

        // Show a diagnostic badge if soft key mask is missing
        if (!skMaskObj && ws)
        {
            ImVec2 tip(barPos.x + 4.0f, barPos.y + 4.0f);
            dl->AddText(tip, IM_COL32(255, 100, 80, 200), "no SKM");
        }

        for (int i = 0; i < numKeys; ++i)
        {
            ImVec2 kPos(barPos.x + 6.0f, barPos.y + keySpacing + i * (keyHeight + keySpacing));
            ImVec2 kSize(keyWidth, keyHeight);
            ImVec2 kMax(kPos.x + keyWidth, kPos.y + keyHeight);

            // InvisibleButton FIRST — sets up ImGui item for hit-testing
            ImGui::SetCursorScreenPos(kPos);
            std::string skLabel = "##sk" + std::to_string(i);
            bool skClicked = ImGui::InvisibleButton(skLabel.c_str(), kSize);
            bool isDown   = ImGui::IsItemActive();
            bool clicked  = ImGui::IsItemClicked(0);
            bool released = ImGui::IsItemDeactivated() || skClicked;
            bool hov      = ImGui::IsItemHovered();

            // Background drawn via draw-list (on top of InvisibleButton)
            ImU32 cellBg = isMuller ? IM_COL32(28, 32, 40, 255) : IM_COL32(42, 47, 58, 255);
            if (hov) cellBg = isDown ? (isMuller ? IM_COL32(14, 88, 185, 255) : IM_COL32(85, 105, 138, 255))
                                     : (isMuller ? IM_COL32(40, 48, 62, 255) : IM_COL32(60, 72, 90, 255));
            dl->AddRectFilled(kPos, kMax, cellBg, 4.0f);

            ImU32 borderCol = isMuller ? (hov ? IM_COL32(14, 135, 250, 255) : IM_COL32(50, 60, 76, 255))
                                       : IM_COL32(85, 92, 108, 255);
            float borderThick = (isMuller && hov) ? 2.0f : 1.2f;
            dl->AddRect(kPos, kMax, borderCol, 4.0f, 0, borderThick);

            if (isMuller)
            {
                // SoftKey position badge in top right: F1..F6
                std::string fBadge = "F" + std::to_string(i + 1);
                dl->AddText(ImVec2(kMax.x - 18.0f, kPos.y + 3.0f), IM_COL32(120, 140, 170, 180), fBadge.c_str());
            }

            if (skMaskObj && i < static_cast<int>(skMaskObj->get_number_children()))
            {
                auto keyId  = skMaskObj->get_child_id(static_cast<std::uint16_t>(i));
                auto keyObj = ws->get_object_by_id(keyId);
                // In case key is referenced through an ObjectPointer
                if (keyObj && keyObj->get_object_type() == isobus::VirtualTerminalObjectType::ObjectPointer)
                {
                    auto ptr = std::dynamic_pointer_cast<isobus::ObjectPointer>(keyObj);
                    if (ptr) keyObj = ws->get_object_by_id(ptr->get_value());
                }

                if (keyObj)
                {
                    auto key = std::dynamic_pointer_cast<isobus::Key>(keyObj);
                    if (key)
                    {
                        ImU32 keyBg = get_imgui_color(ws, key->get_background_color(), cellBg);
                        if (!hov && key->get_background_color() != 254 && (keyBg >> 24) > 0)
                            dl->AddRectFilled(kPos, kMax, keyBg, 4.0f);

                        std::uint8_t code = key->get_key_code();
                        if (code == 0) code = static_cast<std::uint8_t>(i);

                        if (clicked)
                        {
                            vtServer->trigger_macro(keyObj, isobus::EventID::OnKeyPress, isobus::VirtualTerminalObjectType::Key);
                            vtServer->activate_soft_key(keyObj->get_id(), softKeyMaskId, code, true);
                            AudioBeeper::get_instance().play_soft_key_click();
                        }
                        if (released)
                        {
                            vtServer->activate_soft_key(keyObj->get_id(), softKeyMaskId, code, false);
                            vtServer->trigger_macro(keyObj, isobus::EventID::OnKeyRelease, isobus::VirtualTerminalObjectType::Key);
                        }
                    }

                    // Compute bounding box / required design space of key children
                    float minChildX = 99999.0f;
                    float minChildY = 99999.0f;
                    float maxChildX = 0.0f;
                    float maxChildY = 0.0f;
                    bool hasChildren = false;

                    for (std::uint16_t c = 0; c < keyObj->get_number_children(); ++c)
                    {
                        auto cObj = ws->get_object_by_id(keyObj->get_child_id(c));
                        if (cObj)
                        {
                            hasChildren = true;
                            float cx = static_cast<float>(keyObj->get_child_x(c));
                            float cy = static_cast<float>(keyObj->get_child_y(c));
                            float cw = static_cast<float>(cObj->get_width());
                            float ch = static_cast<float>(cObj->get_height());
                            minChildX = std::min(minChildX, cx);
                            minChildY = std::min(minChildY, cy);
                            maxChildX = std::max(maxChildX, cx + cw);
                            maxChildY = std::max(maxChildY, cy + ch);
                        }
                    }

                    float skScale = 1.0f;
                    float pad = 4.0f;
                    float availW = keyWidth - pad * 2.0f;
                    float availH = keyHeight - pad * 2.0f;
                    ImVec2 renderOrigin = kPos;

                    if (hasChildren && availW > 0.0f && availH > 0.0f)
                    {
                        // Symmetric design width/height: if content starts at minChildX, assume centered in canvas
                        float designW = std::max(maxChildX, minChildX + maxChildX);
                        float designH = std::max(maxChildY, minChildY + maxChildY);

                        if (designW > availW || designH > availH)
                        {
                            float scaleX = (designW > 0.0f) ? (availW / designW) : 1.0f;
                            float scaleY = (designH > 0.0f) ? (availH / designH) : 1.0f;
                            skScale = std::min(scaleX, scaleY);
                        }

                        float contentW = designW * skScale;
                        float contentH = designH * skScale;
                        float offX = std::max(0.0f, (keyWidth - contentW) * 0.5f);
                        float offY = std::max(0.0f, (keyHeight - contentH) * 0.5f);
                        renderOrigin = ImVec2(kPos.x + offX, kPos.y + offY);
                    }

                    // Clip key children to the key cell
                    dl->PushClipRect(kPos, kMax, true);
                    for (std::uint16_t c = 0; c < keyObj->get_number_children(); ++c)
                    {
                        auto cObj = ws->get_object_by_id(keyObj->get_child_id(c));
                        if (cObj)
                            render_object(dl, renderOrigin, ws, cObj,
                                          keyObj->get_child_x(c), keyObj->get_child_y(c),
                                          keyObj->get_id(), skScale);
                    }
                    dl->PopClipRect();
                }
            }

            // F-key label (subtle, bottom-right corner)
            std::string lbl = "F" + std::to_string(i + 1);
            ImVec2 lblSz = ImGui::CalcTextSize(lbl.c_str());
            dl->AddText(ImVec2(kMax.x - lblSz.x - 3.0f, kMax.y - lblSz.y - 2.0f),
                        IM_COL32(80, 90, 110, 200), lbl.c_str());
        }
    }

    // ─── Modal dialogs ────────────────────────────────────────────────────────────

    void VTRenderer::render_modals()
    {
        auto ws = vtServer->get_active_ws();

        // ── InputList Dropdown Modal ─────────────────────────────────────────
        if (showInputListPopup)
        {
            auto inList = ws ? std::dynamic_pointer_cast<isobus::InputList>(ws->get_object_by_id(editingListObjectId)) : nullptr;
            if (inList)
            {
                ImGui::OpenPopup("Select List Item");
                if (ImGui::BeginPopupModal("Select List Item", &showInputListPopup, ImGuiWindowFlags_AlwaysAutoResize))
                {
                    ImGui::Text("Input List (Object ID: %u)", editingListObjectId);
                    ImGui::Separator();

                    std::uint32_t selectedIndex = inList->get_value();
                    auto varId = inList->get_variable_reference();
                    if (varId != isobus::NULL_OBJECT_ID)
                    {
                        auto varObj = std::dynamic_pointer_cast<isobus::NumberVariable>(ws->get_object_by_id(varId));
                        if (varObj) selectedIndex = varObj->get_value();
                    }

                    for (std::uint16_t i = 0; i < inList->get_number_children(); ++i)
                    {
                        auto childObj = ws->get_object_by_id(inList->get_child_id(i));
                        std::string label = "Item " + std::to_string(i + 1);
                        if (childObj)
                        {
                            if (childObj->get_object_type() == isobus::VirtualTerminalObjectType::OutputString)
                            {
                                label = std::dynamic_pointer_cast<isobus::OutputString>(childObj)->displayed_value(ws->get_object_tree());
                            }
                        }

                        // ISO 11783-6: Value is 1-based index (1 = first item)
                        std::uint32_t itemValue = i + 1;
                        bool isSel = (itemValue == selectedIndex);
                        if (ImGui::Selectable(label.c_str(), isSel))
                        {
                            vtServer->trigger_macro(inList, isobus::EventID::OnEntryOfAValue, isobus::VirtualTerminalObjectType::InputList);
                            vtServer->trigger_macro(inList, isobus::EventID::OnEntryOfANewValue, isobus::VirtualTerminalObjectType::InputList);

                            if (varId != isobus::NULL_OBJECT_ID)
                            {
                                auto numVar = std::dynamic_pointer_cast<isobus::NumberVariable>(ws->get_object_by_id(varId));
                                if (numVar)
                                {
                                    numVar->set_value(itemValue);
                                    vtServer->change_numeric_value(numVar->get_id(), itemValue);
                                    vtServer->trigger_macro(numVar, isobus::EventID::OnChangeValue, isobus::VirtualTerminalObjectType::NumberVariable);
                                }
                            }
                            else
                            {
                                inList->set_value(static_cast<std::uint8_t>(itemValue));
                                vtServer->change_numeric_value(inList->get_id(), itemValue);
                                vtServer->trigger_macro(inList, isobus::EventID::OnChangeValue, isobus::VirtualTerminalObjectType::InputList);
                            }
                            showInputListPopup = false;
                            ImGui::CloseCurrentPopup();
                        }
                    }

                    ImGui::Separator();
                    if (ImGui::Button("Cancel", ImVec2(120, 0)))
                    {
                        showInputListPopup = false;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
            }
            else
            {
                showInputListPopup = false;
            }
        }

        if (showNumberKeypad)
        {
            ImGui::OpenPopup("Enter Numeric Value");
            if (ImGui::BeginPopupModal("Enter Numeric Value", &showNumberKeypad, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::Text("Object ID: %u", editingNumberObjectId);
                ImGui::Separator();
                ImGui::InputInt("Value", &editingNumberValue);
                if (ImGui::Button("OK", ImVec2(100, 0)))
                {
                    auto inNum = ws ? std::dynamic_pointer_cast<isobus::InputNumber>(ws->get_object_by_id(editingNumberObjectId)) : nullptr;
                    if (inNum)
                    {
                        vtServer->trigger_macro(inNum, isobus::EventID::OnEntryOfAValue, isobus::VirtualTerminalObjectType::InputNumber);
                        auto varId = inNum->get_variable_reference();
                        if (varId != isobus::NULL_OBJECT_ID && ws)
                        {
                            auto numVar = std::dynamic_pointer_cast<isobus::NumberVariable>(ws->get_object_by_id(varId));
                            if (numVar)
                            {
                                numVar->set_value(static_cast<std::uint32_t>(editingNumberValue));
                                vtServer->change_numeric_value(numVar->get_id(), static_cast<std::uint32_t>(editingNumberValue));
                                vtServer->trigger_macro(numVar, isobus::EventID::OnChangeValue, isobus::VirtualTerminalObjectType::NumberVariable);
                            }
                        }
                        else
                        {
                            inNum->set_value(static_cast<std::uint32_t>(editingNumberValue));
                            vtServer->change_numeric_value(inNum->get_id(), static_cast<std::uint32_t>(editingNumberValue));
                            vtServer->trigger_macro(inNum, isobus::EventID::OnChangeValue, isobus::VirtualTerminalObjectType::InputNumber);
                        }
                    }
                    else
                    {
                        vtServer->change_numeric_value(editingNumberObjectId, static_cast<std::uint32_t>(editingNumberValue));
                    }
                    showNumberKeypad = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(100, 0)))
                {
                    showNumberKeypad = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        if (showStringEditor)
        {
            ImGui::OpenPopup("Enter Text String");
            if (ImGui::BeginPopupModal("Enter Text String", &showStringEditor, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::Text("Object ID: %u", editingStringObjectId);
                ImGui::Separator();
                ImGui::InputText("String", editingStringBuffer, sizeof(editingStringBuffer));
                if (ImGui::Button("OK", ImVec2(100, 0)))
                {
                    auto inStr = ws ? std::dynamic_pointer_cast<isobus::InputString>(ws->get_object_by_id(editingStringObjectId)) : nullptr;
                    if (inStr)
                    {
                        vtServer->trigger_macro(inStr, isobus::EventID::OnEntryOfAValue, isobus::VirtualTerminalObjectType::InputString);
                        auto varId = inStr->get_variable_reference();
                        if (varId != isobus::NULL_OBJECT_ID && ws)
                        {
                            auto strVar = std::dynamic_pointer_cast<isobus::StringVariable>(ws->get_object_by_id(varId));
                            if (strVar)
                            {
                                strVar->set_value(std::string(editingStringBuffer));
                                vtServer->change_string_value(strVar->get_id(), std::string(editingStringBuffer));
                                vtServer->trigger_macro(strVar, isobus::EventID::OnChangeValue, isobus::VirtualTerminalObjectType::StringVariable);
                            }
                        }
                        else
                        {
                            inStr->set_value(std::string(editingStringBuffer));
                            vtServer->change_string_value(inStr->get_id(), std::string(editingStringBuffer));
                            vtServer->trigger_macro(inStr, isobus::EventID::OnChangeValue, isobus::VirtualTerminalObjectType::InputString);
                        }
                    }
                    else
                    {
                        vtServer->change_string_value(editingStringObjectId, std::string(editingStringBuffer));
                    }
                    showStringEditor = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(100, 0)))
                {
                    showStringEditor = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }
    }

    // ─── Settings Dialogs ─────────────────────────────────────────────────────────

    void VTRenderer::render_settings_dialogs()
    {
        // ── Main Terminal & System Settings Dialog (Commercial Console Style) ──
        if (showMainSettingsDialog)
        {
            ImGui::SetNextWindowSize(ImVec2(820.0f, 640.0f), ImGuiCond_FirstUseEver);
            if (ImGui::Begin("Konfiguracja Terminala ISOBUS & Systemu (Setup)", &showMainSettingsDialog, ImGuiWindowFlags_NoCollapse))
            {
                auto &ts = TerminalSettings::get_instance();
                auto &cfg = ts.get_config();

                if (ImGui::BeginTabBar("MainSettingsTabs", ImGuiTabBarFlags_None))
                {
                    // ── Tab 1: 📡 Sprzęt & Magistrala CAN ──
                    if (ImGui::BeginTabItem(" 📡 CAN & Sprzet "))
                    {
                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Konfiguracja magistrali CAN i interfejsu sprzetowego:");
                        ImGui::Separator();

                        const char *drivers[] = { "PEAK PCAN-USB (Sprzetowy)", "Wirtualny Loopback CAN (Symulacja)" };
                        ImGui::SetNextItemWidth(320.0f);
                        if (ImGui::Combo("Sterownik CAN", &cfg.canDriver, drivers, 2))
                        {
                            if (canManager)
                            {
                                auto dt = (cfg.canDriver == 0) ? CanDriverType::PeakPcan : CanDriverType::VirtualLoopback;
                                canManager->initialize(dt, cfg.pcanChannel);
                            }
                        }

                        if (cfg.canDriver == 0)
                        {
                            const char *pcanChannels[] = {
                                "PCAN_USBBUS1 (0x51 - Domyslny)",
                                "PCAN_USBBUS2 (0x52)",
                                "PCAN_USBBUS3 (0x53)",
                                "PCAN_USBBUS4 (0x54)",
                                "PCAN_USBBUS5 (0x55)",
                                "PCAN_USBBUS6 (0x56)",
                                "PCAN_USBBUS7 (0x57)",
                                "PCAN_USBBUS8 (0x58)"
                            };
                            int chIdx = cfg.pcanChannel - 81;
                            if (chIdx < 0 || chIdx > 7) chIdx = 0;
                            ImGui::SetNextItemWidth(320.0f);
                            if (ImGui::Combo("Kanal PCAN-USB", &chIdx, pcanChannels, 8))
                            {
                                cfg.pcanChannel = 81 + chIdx;
                                if (canManager)
                                {
                                    canManager->initialize(CanDriverType::PeakPcan, cfg.pcanChannel);
                                }
                            }
                        }

                        const char *baudRates[] = { "250 kbit/s (Standard ISOBUS ISO 11783)", "500 kbit/s (Szybki CAN)" };
                        int baudIdx = (cfg.canBaudRate == 500000) ? 1 : 0;
                        ImGui::SetNextItemWidth(320.0f);
                        if (ImGui::Combo("Predkosc transmisji", &baudIdx, baudRates, 2))
                        {
                            cfg.canBaudRate = (baudIdx == 1) ? 500000 : 250000;
                        }

                        ImGui::Checkbox("Automatyczne wznawianie polaczenia po bledzie (Auto-reconnect)", &cfg.autoReconnect);

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        // CAN Live Status display
                        ImGui::Text("Status magistrali CAN:");
                        if (canManager)
                        {
                            bool conn = canManager->is_connected();
                            ImGui::SameLine();
                            if (conn)
                                ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "[ POLACZONY - OK ]");
                            else
                                ImGui::TextColored(ImVec4(0.9f, 0.2f, 0.2f, 1.0f), "[ ROZLACZONY / BRAK URZADZENIA ]");

                            ImGui::BulletText("Odebrane ramki (RX): %llu  |  Wyslane (TX): %llu",
                                              (unsigned long long)canManager->get_rx_count(),
                                              (unsigned long long)canManager->get_tx_count());
                            ImGui::BulletText("Obciazenie magistrali: %.1f%%  |  Predkosc ramek: %.0f fps",
                                              canManager->get_bus_load(), canManager->get_fps());

                            std::string err = canManager->get_last_error();
                            if (!err.empty())
                            {
                                ImGui::BulletText("Ostatni komunikat sterownika: %s", err.c_str());
                            }

                            ImGui::Spacing();
                            if (ImGui::Button(" ⚡ Polacz ponownie z magistrala CAN ", ImVec2(320, 32)))
                            {
                                auto dt = (cfg.canDriver == 0) ? CanDriverType::PeakPcan : CanDriverType::VirtualLoopback;
                                canManager->initialize(dt, cfg.pcanChannel);
                                isobus::CANNetworkManager::CANNetwork.send_request_for_address_claim(0);
                                ts.play_touch_beep();
                            }
                        }

                        ImGui::EndTabItem();
                    }

                    // ── Tab 2: 🖥️ Ekran & Klawisze VT ──
                    if (ImGui::BeginTabItem(" 🖥️ Ekran & Klawisze VT "))
                    {
                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Szybkie presety ekranu (Rozdzielczosc zgodna z terminalami komercyjnymi):");

                        if (ImGui::Button(" 480x480 (Standard ISOBUS) "))
                        {
                            cfg.dataMaskWidth = 480; cfg.dataMaskHeight = 480; cfg.dataMaskSize = 480;
                            vtServer->set_data_mask_size(480);
                            ts.play_touch_beep();
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(" 640x480 (VGA Rozszerzony) "))
                        {
                            cfg.dataMaskWidth = 640; cfg.dataMaskHeight = 480; cfg.dataMaskSize = 640;
                            vtServer->set_data_mask_size(640);
                            ts.play_touch_beep();
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(" 800x480 (CommandCenter / GFX) "))
                        {
                            cfg.dataMaskWidth = 800; cfg.dataMaskHeight = 480; cfg.dataMaskSize = 800;
                            vtServer->set_data_mask_size(800);
                            ts.play_touch_beep();
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(" 800x600 (SVGA HD) "))
                        {
                            cfg.dataMaskWidth = 800; cfg.dataMaskHeight = 600; cfg.dataMaskSize = 800;
                            vtServer->set_data_mask_size(800);
                            ts.play_touch_beep();
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();

                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Wymiary maski danych (Data Mask):");
                        int maskPx = cfg.dataMaskSize;
                        ImGui::SetNextItemWidth(120.0f);
                        if (ImGui::InputInt("Rozmiar maski (px)##dm", &maskPx, 16, 64))
                        {
                            maskPx = std::max(60, std::min(1920, maskPx));
                            cfg.dataMaskSize = maskPx;
                            cfg.dataMaskWidth = maskPx;
                            cfg.dataMaskHeight = maskPx;
                            vtServer->set_data_mask_size(static_cast<std::uint16_t>(maskPx));
                        }

                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Uklad klawiszy funkcyjnych (Soft Keys):");
                        const char *skPositions[] = { "Po lewej stronie", "Po prawej stronie (Domyslnie)" };
                        int skPosIdx = cfg.softKeyOnRight ? 1 : 0;
                        ImGui::SetNextItemWidth(260.0f);
                        if (ImGui::Combo("Polozenie paska klawiszy", &skPosIdx, skPositions, 2))
                        {
                            cfg.softKeyOnRight = (skPosIdx == 1);
                        }

                        int skCols = cfg.skColumns;
                        ImGui::SetNextItemWidth(120.0f);
                        if (ImGui::InputInt("Liczba kolumn klawiszy (1-2)", &skCols, 1, 1))
                        {
                            cfg.skColumns = std::max(1, std::min(2, skCols));
                            vtServer->set_sk_columns(static_cast<std::uint8_t>(cfg.skColumns));
                        }

                        int skRows = cfg.skRows;
                        ImGui::SetNextItemWidth(120.0f);
                        if (ImGui::InputInt("Liczba wierszy klawiszy (np. 6 lub 8)", &skRows, 1, 1))
                        {
                            cfg.skRows = std::max(1, std::min(16, skRows));
                            vtServer->set_sk_rows(static_cast<std::uint8_t>(cfg.skRows));
                        }

                        int skW = cfg.skDescWidth;
                        int skH = cfg.skDescHeight;
                        ImGui::SetNextItemWidth(120.0f);
                        if (ImGui::InputInt("Szerokosc klawisza (px)", &skW, 5, 10))
                        {
                            cfg.skDescWidth = std::max(40, std::min(255, skW));
                            vtServer->set_sk_desc_width(static_cast<std::uint8_t>(cfg.skDescWidth));
                        }
                        ImGui::SetNextItemWidth(120.0f);
                        if (ImGui::InputInt("Wysokosc klawisza (px)", &skH, 5, 10))
                        {
                            cfg.skDescHeight = std::max(40, std::min(255, skH));
                            vtServer->set_sk_desc_height(static_cast<std::uint8_t>(cfg.skDescHeight));
                        }

                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Wyglad i auto-skalowanie grafiki:");
                        const char *bezStyles[] = {
                            "M\xc3\x9cller-Elektronik TOUCH800 (Grafit / Niebieski akcent ME / Status Bar)",
                            "Nowoczesny Glass (Sciete rogi / szklany akcent)",
                            "Minimalistyczny Frameless (Cienka ramka)",
                            "Traktorowy Rugged (Ciemny grafit / masywna ramka)"
                        };
                        ImGui::SetNextItemWidth(360.0f);
                        ImGui::Combo("Styl obudowy terminala", &cfg.bezelStyle, bezStyles, 4);

                        ImGui::Checkbox("Automatyczne dopasowanie grafik klawiszy (Auto-Scale SoftKeys)", &cfg.autoScaleSoftKeys);
                        ImGui::Checkbox("Automatyczne dopasowanie maski danych (Auto-Scale DataMask)", &cfg.autoScaleDataMask);

                        ImGui::EndTabItem();
                    }

                    // ── Tab 3: 🛰️ AgOpenGPS & Task Controller ──
                    if (ImGui::BeginTabItem(" 🛰️ AgOpenGPS & TC "))
                    {
                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Integracja z Task Controllerem (TC-BAS / TC-SC) i AgOpenGPS:");
                        ImGui::Separator();

                        ImGui::Checkbox("Wlacz Task Controller (TC-BAS / TC-SC / TC-GEO na CAN)", &cfg.tcEnabled);
                        ImGui::Checkbox("Automatycznie przypisuj sekcje belki z DDOP maszyny", &cfg.autoBoomSections);

                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Konfiguracja portow sieciowych mostka UDP:");

                        ImGui::SetNextItemWidth(140.0f);
                        ImGui::InputInt("Port nasluchu z AOG/AGIO (Rx)##aog", &cfg.aogRxPort);

                        ImGui::SetNextItemWidth(140.0f);
                        ImGui::InputInt("Port nadawania do AgOpenGPS (Tx)##aog", &cfg.aogTxPort);

                        char subnetBuf[32];
                        strncpy_s(subnetBuf, cfg.aogSubnet.c_str(), sizeof(subnetBuf));
                        ImGui::SetNextItemWidth(180.0f);
                        if (ImGui::InputText("Podsiec UDP (np. 192.168.5 lub 127.0.0)", subnetBuf, sizeof(subnetBuf)))
                        {
                            cfg.aogSubnet = subnetBuf;
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("(Rozglaszanie na %s.255)", cfg.aogSubnet.c_str());

                        if (tcManager)
                        {
                            ImGui::Spacing();
                            ImGui::Separator();
                            ImGui::Spacing();
                            ImGui::Text("Status polaczenia AgOpenGPS:");
                            ImGui::SameLine();
                            if (tcManager->is_aog_connected())
                                ImGui::TextColored(ImVec4(0.2f, 0.9f, 0.2f, 1.0f), "[ POLACZONY ]");
                            else
                                ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1.0f), "[ OCZEKIWANIE NA AGIO / AGOPENGPS ]");

                            ImGui::BulletText("Predkosc ciagnika z GPS: %.1f km/h", tcManager->get_speed_kph());
                            ImGui::BulletText("Wykryte sekcje implementu: %u sekcji", tcManager->get_num_sections());
                        }

                        ImGui::EndTabItem();
                    }

                    // ── Tab 4: 🔔 Dźwięk & Dotyk ──
                    if (ImGui::BeginTabItem(" 🔔 Dzwiek & Dotyk "))
                    {
                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Sygnalizacja dzwiekowa i dotykowa konsoli:");
                        ImGui::Separator();

                        ImGui::Checkbox("Dzwiek klikniecia przyciskow i klawiszy (Touch Tactile Beep)", &cfg.touchBeepEnabled);
                        ImGui::SameLine();
                        if (ImGui::Button(" 🎵 Test klikniecia "))
                        {
                            ts.play_touch_beep();
                        }

                        ImGui::Spacing();
                        ImGui::Checkbox("Wycisz brzeczyk glosnego alarmu (Mute Alarm Sound)", &cfg.alarmSoundMuted);
                        ImGui::SameLine();
                        if (ImGui::Button(" 🔊 Test brzeczyka alarmu "))
                        {
                            ts.play_alarm_beep();
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Motyw kolorystyczny i skala dotyku:");
                        if (ImGui::Checkbox("Tryb dzienny (Jasny motyw)", &cfg.isDayMode))
                        {
                            if (cfg.isDayMode) ImGui::StyleColorsLight();
                            else ImGui::StyleColorsDark();
                        }
                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Tryb Nakladki HUD na AgOpenGPS:");
                        const char* dockSideOptions[] = { "Dokuj do Prawej", "Dokuj do Lewej", "Okno Plywajace" };
                        ImGui::SetNextItemWidth(200.0f);
                        ImGui::Combo("Pozycja dokowania HUD", &cfg.hudDockSide, dockSideOptions, 3);
                        ImGui::SetNextItemWidth(200.0f);
                        ImGui::SliderFloat("Przezroczystosc HUD", &cfg.hudOpacity, 0.40f, 1.0f, "%.0f%%");
                        ImGui::Checkbox("Zawsze na wierzchu (Always-On-Top)", &cfg.hudAlwaysOnTop);

                        ImGui::EndTabItem();
                    }

                    // ── Tab 5: 🌐 Język & Jednostki ──
                    if (ImGui::BeginTabItem(" 🌐 Jezyk & Jednostki "))
                    {
                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Standard ISO 11783-7 Localization & Units:");
                        ImGui::Separator();

                        if (ImGui::Button(" 🇵🇱 Profil: Polska / Europa (Metryczny, dd.mm.yyyy, przecinek) "))
                        {
                            cfg.langCode = "pl";
                            cfg.countryCode = "PL";
                            cfg.unitArea = 0;
                            cfg.unitDate = 0;
                            cfg.unitDecimal = 0;
                            cfg.unitDistance = 0;
                            cfg.unitForce = 0;
                            cfg.unitGeneric = 0;
                            cfg.unitMass = 0;
                            cfg.unitPressure = 0;
                            cfg.unitTemperature = 0;
                            cfg.unitTime = 0;
                            cfg.unitVolume = 0;

                            langCode[0] = 'p'; langCode[1] = 'l'; langCode[2] = '\0';
                            countryCode[0] = 'P'; countryCode[1] = 'L'; countryCode[2] = '\0';
                            unitArea = 0; unitDate = 0; unitDecimal = 0; unitDistance = 0;
                            unitForce = 0; unitGeneric = 0; unitMass = 0; unitPressure = 0;
                            unitTemperature = 0; unitTime = 0; unitVolume = 0;
                            ts.play_touch_beep();
                        }
                        ImGui::SameLine();
                        if (ImGui::Button(" 🇺🇸 Profil: USA / Imperial (Imperialny, kropka) "))
                        {
                            cfg.langCode = "en";
                            cfg.countryCode = "US";
                            cfg.unitArea = 1;
                            cfg.unitDate = 2;
                            cfg.unitDecimal = 1;
                            cfg.unitDistance = 1;
                            cfg.unitForce = 1;
                            cfg.unitGeneric = 1;
                            cfg.unitMass = 1;
                            cfg.unitPressure = 1;
                            cfg.unitTemperature = 1;
                            cfg.unitTime = 0;
                            cfg.unitVolume = 1;

                            langCode[0] = 'e'; langCode[1] = 'n'; langCode[2] = '\0';
                            countryCode[0] = 'U'; countryCode[1] = 'S'; countryCode[2] = '\0';
                            unitArea = 1; unitDate = 2; unitDecimal = 1; unitDistance = 1;
                            unitForce = 1; unitGeneric = 1; unitMass = 1; unitPressure = 1;
                            unitTemperature = 1; unitTime = 0; unitVolume = 1;
                            ts.play_touch_beep();
                        }

                        ImGui::Spacing();
                        ImGui::SetNextItemWidth(80.0f);
                        if (ImGui::InputText("Kod jezyka (2 znaki)", langCode, 3))
                        {
                            cfg.langCode = langCode;
                        }
                        ImGui::SetNextItemWidth(80.0f);
                        if (ImGui::InputText("Kod kraju (2 znaki)", countryCode, 3))
                        {
                            cfg.countryCode = countryCode;
                        }

                        ImGui::Spacing();
                        const char* areaU[]  = { "metryczne (hektary ha)", "imperialne (akry acre)" };
                        const char* dateU[]  = { "dd.mm.yyyy", "dd.yyyy.mm", "mm.dd.yyyy", "yyyy.mm.dd", "yyyy.dd.mm", "mm.yyyy.dd" };
                        const char* decU[]   = { "przecinek (,)", "kropka (.)" };
                        const char* distU[]  = { "metryczne (km, m, cm)", "imperialne (mile, stopy, cale)" };
                        const char* massU[]  = { "metryczne (kg, t)", "imperialne (funty lb)" };
                        const char* pressU[] = { "metryczne (bar, kPa)", "imperialne (psi)" };
                        const char* tempU[]  = { "metryczne (Celsjusz °C)", "imperialne (Fahrenheit °F)" };
                        const char* volU[]   = { "metryczne (litry l)", "imperialne (galony gal)" };

                        ImGui::SetNextItemWidth(260.0f); if (ImGui::Combo("Powierzchnia", &unitArea, areaU, 2)) cfg.unitArea = unitArea;
                        ImGui::SetNextItemWidth(260.0f); if (ImGui::Combo("Format daty", &unitDate, dateU, 6)) cfg.unitDate = unitDate;
                        ImGui::SetNextItemWidth(260.0f); if (ImGui::Combo("Separator dziesietny", &unitDecimal, decU, 2)) cfg.unitDecimal = unitDecimal;
                        ImGui::SetNextItemWidth(260.0f); if (ImGui::Combo("Odleglosc", &unitDistance, distU, 2)) cfg.unitDistance = unitDistance;
                        ImGui::SetNextItemWidth(260.0f); if (ImGui::Combo("Masa", &unitMass, massU, 2)) cfg.unitMass = unitMass;
                        ImGui::SetNextItemWidth(260.0f); if (ImGui::Combo("Cisnienie", &unitPressure, pressU, 2)) cfg.unitPressure = unitPressure;
                        ImGui::SetNextItemWidth(260.0f); if (ImGui::Combo("Temperatura", &unitTemperature, tempU, 2)) cfg.unitTemperature = unitTemperature;
                        ImGui::SetNextItemWidth(260.0f); if (ImGui::Combo("Objetosc cieczy", &unitVolume, volU, 2)) cfg.unitVolume = unitVolume;

                        ImGui::EndTabItem();
                    }

                    // ── Tab 6: 💾 Pamięć maszyn & Pula IOP ──
                    if (ImGui::BeginTabItem(" 💾 Pamiec maszyn & Cache "))
                    {
                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Szybki start maszyn (Object Pool Cache na dysku):");
                        ImGui::Separator();

                        std::size_t poolCount = ts.get_cached_pools_count();
                        std::uint64_t totalBytes = ts.get_cached_pools_total_bytes();
                        float totalKb = static_cast<float>(totalBytes) / 1024.0f;

                        ImGui::BulletText("Liczba zapamietanych maszyn w pamieci trwalej: %zu", poolCount);
                        ImGui::BulletText("Laczny rozmiar plikow puli obiektow: %.1f KB (%llu bajtow)", totalKb, (unsigned long long)totalBytes);

                        auto poolNames = ts.get_cached_pool_names();
                        if (!poolNames.empty())
                        {
                            ImGui::Spacing();
                            ImGui::Text("Zapamietane pule obiektow w katalogu cache:");
                            ImGui::BeginChild("PoolNamesChild", ImVec2(0, 140), true);
                            for (const auto &pn : poolNames)
                            {
                                ImGui::BulletText("%s", pn.c_str());
                            }
                            ImGui::EndChild();
                        }

                        ImGui::Spacing();
                        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                            "Wskazowka: Jesli modyfikujesz program maszyny (np. wgrywasz nowy kod do ESP32),\n"
                            "wyczysc pamiec podreczna, aby terminal pobral nowa pule obiektow zamiast wczytywac stara.");

                        if (ImGui::Button(" 🗑️ Wyczysc pamiec podreczna maszyn (Clear Cache) ", ImVec2(360, 32)))
                        {
                            ts.clear_object_pool_cache();
                            ts.play_touch_beep();
                        }

                        ImGui::EndTabItem();
                    }

                    ImGui::EndTabBar();
                }

                // ── Footer: Action & Persistence Buttons ──
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();

                // Notification banner when saved
                if (settingsSavedNotification)
                {
                    settingsSavedTimer -= ImGui::GetIO().DeltaTime;
                    if (settingsSavedTimer <= 0.0f)
                    {
                        settingsSavedNotification = false;
                    }
                    else
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.3f, 1.0f));
                        ImGui::Text("✓ USTAWIENIA ZAPISANE! Plik vt_settings.ini zaktualizowany. Aplikacja wlaczy sie juz z tymi ustawieniami!");
                        ImGui::PopStyleColor();
                        ImGui::Spacing();
                    }
                }

                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.22f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.70f, 0.30f, 1.0f));
                if (ImGui::Button(" 💾 Zapisz jako domyslne (Save Defaults) ", ImVec2(280, 36)))
                {
                    save_settings();
                    ts.save();
                    settingsSavedNotification = true;
                    settingsSavedTimer = 3.5f;
                    ts.play_touch_beep();
                }
                ImGui::PopStyleColor(2);

                ImGui::SameLine();
                if (ImGui::Button(" ↺ Przywroc fabryczne ", ImVec2(180, 36)))
                {
                    ts.reset_to_defaults();
                    load_settings();
                    ts.save();
                    settingsSavedNotification = true;
                    settingsSavedTimer = 2.0f;
                    ts.play_touch_beep();
                }

                ImGui::SameLine();
                if (ImGui::Button(" Zamknij ", ImVec2(120, 36)))
                {
                    showMainSettingsDialog = false;
                }

                ImGui::End();
            }
        }

        // ── VT Capabilities ───────────────────────────────────────────────────
        if (showVTCapabilitiesDialog)
            ImGui::OpenPopup("VT Capabilities##cap");

        if (ImGui::BeginPopupModal("VT Capabilities##cap", &showVTCapabilitiesDialog,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("VT Server Capabilities");
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                "(!) Zmiana ustawien wymaga restartu ECU / reconnect.");
            ImGui::Separator();

            // VT Version
            using VTV = isobus::VirtualTerminalBase::VTVersion;
            const char* vtVerNames[] = {
                "Version 2 or Older", "Version 3", "Version 4", "Version 5", "Version 6"
            };
            const VTV vtVerVals[] = {
                VTV::Version2OrOlder, VTV::Version3, VTV::Version4, VTV::Version5, VTV::Version6
            };
            VTV curVer = vtServer->get_configured_version();
            int curVerIdx = 2;
            for (int i = 0; i < 5; ++i)
                if (vtVerVals[i] == curVer) { curVerIdx = i; break; }

            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::Combo("VT Version", &curVerIdx, vtVerNames, 5))
                vtServer->set_vt_version(vtVerVals[curVerIdx]);

            ImGui::Separator();

            // Data Mask Size
            int maskPx = static_cast<int>(vtServer->get_configured_mask_size());
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::InputInt("Data Mask Size px (kwadrat)", &maskPx, 8, 32))
            {
                maskPx = std::max(60, std::min(1024, maskPx));
                vtServer->set_data_mask_size(static_cast<std::uint16_t>(maskPx));
                save_settings();
            }

            ImGui::Separator();

            // SK descriptor dimensions
            int skW = static_cast<int>(vtServer->get_configured_sk_width());
            int skH = static_cast<int>(vtServer->get_configured_sk_height());
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::InputInt("SK Designator Width px (min 60)", &skW, 1, 10))
            {
                skW = std::max(60, skW);
                vtServer->set_sk_desc_width(static_cast<std::uint8_t>(std::min(skW, 255)));
                save_settings();
            }
            ImGui::SetNextItemWidth(110.0f);
            if (ImGui::InputInt("SK Designator Height px (min 60)", &skH, 1, 10))
            {
                skH = std::max(60, skH);
                vtServer->set_sk_desc_height(static_cast<std::uint8_t>(std::min(skH, 255)));
                save_settings();
            }

            ImGui::Separator();

            // SK layout
            int skCols = static_cast<int>(vtServer->get_configured_sk_columns());
            int skRows2 = static_cast<int>(vtServer->get_configured_sk_rows());
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::InputInt("SK Columns (min 1)", &skCols, 1, 1))
            {
                skCols = std::max(1, skCols);
                vtServer->set_sk_columns(static_cast<std::uint8_t>(std::min(skCols, 8)));
                save_settings();
            }
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::InputInt("SK Rows (min 1)", &skRows2, 1, 1))
            {
                skRows2 = std::max(1, skRows2);
                vtServer->set_sk_rows(static_cast<std::uint8_t>(std::min(skRows2, 16)));
                save_settings();
            }

            ImGui::Separator();
            if (ImGui::Button("Zamknij##cap", ImVec2(120, 0)))
            {
                save_settings();
                showVTCapabilitiesDialog = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // ── Language / Units ──────────────────────────────────────────────────
        if (showLanguageDialog)
            ImGui::OpenPopup("Language & Units##lang");

        if (ImGui::BeginPopupModal("Language & Units##lang", &showLanguageDialog,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("Ustawienia jezyka i jednostek (ISO 11783-7)");
            ImGui::Separator();

            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::InputText("Language Code (2 znaki)", langCode, 3)) save_settings();
            ImGui::SetNextItemWidth(80.0f);
            if (ImGui::InputText("Country Code (2 znaki)", countryCode, 3)) save_settings();

            ImGui::Separator();
            ImGui::Text("Jednostki:");

            const char* areaU[]  = { "metryczne", "imperialne/US" };
            const char* dateU[]  = { "dd.mm.yyyy", "dd.yyyy.mm", "mm.dd.yyyy", "yyyy.mm.dd", "yyyy.dd.mm", "mm.yyyy.dd" };
            const char* decU[]   = { "przecinek (,)", "kropka (.)" };
            const char* distU[]  = { "metryczne", "imperialne/US" };
            const char* forceU[] = { "metryczne", "imperialne/US" };
            const char* genU[]   = { "metryczne", "imperialne/US" };
            const char* massU[]  = { "metryczne", "imperialne/US" };
            const char* pressU[] = { "metryczne (Pa/bar)", "imperialne (psi)" };
            const char* tempU[]  = { "metryczne (C/K)", "imperialne (F)" };
            const char* timeU[]  = { "h:mm:ss", "h:mm" };
            const char* volU[]   = { "metryczne", "imperialne/US" };

            ImGui::SetNextItemWidth(200.0f); if (ImGui::Combo("Powierzchnia",   &unitArea,        areaU,  2)) save_settings();
            ImGui::SetNextItemWidth(200.0f); if (ImGui::Combo("Format daty",    &unitDate,        dateU,  6)) save_settings();
            ImGui::SetNextItemWidth(200.0f); if (ImGui::Combo("Separatordz.",   &unitDecimal,     decU,   2)) save_settings();
            ImGui::SetNextItemWidth(200.0f); if (ImGui::Combo("Odleglosc",      &unitDistance,    distU,  2)) save_settings();
            ImGui::SetNextItemWidth(200.0f); if (ImGui::Combo("Sila",           &unitForce,       forceU, 2)) save_settings();
            ImGui::SetNextItemWidth(200.0f); if (ImGui::Combo("Ogolne",         &unitGeneric,     genU,   2)) save_settings();
            ImGui::SetNextItemWidth(200.0f); if (ImGui::Combo("Masa",           &unitMass,        massU,  2)) save_settings();
            ImGui::SetNextItemWidth(200.0f); if (ImGui::Combo("Cisnienie",      &unitPressure,    pressU, 2)) save_settings();
            ImGui::SetNextItemWidth(200.0f); if (ImGui::Combo("Temperatura",    &unitTemperature, tempU,  2)) save_settings();
            ImGui::SetNextItemWidth(200.0f); if (ImGui::Combo("Czas",           &unitTime,        timeU,  2)) save_settings();
            ImGui::SetNextItemWidth(200.0f); if (ImGui::Combo("Objetosc",       &unitVolume,      volU,   2)) save_settings();

            ImGui::Separator();
            if (ImGui::Button("Zamknij##lang", ImVec2(120, 0)))
            {
                save_settings();
                showLanguageDialog = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    void VTRenderer::render_mini_views_window(bool *p_open)
    {
        if (p_open && !*p_open) return;

        ImGui::SetNextWindowSize(ImVec2(340, 520), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Podglad Maszyn w Tle (Mini-Views)###MiniViews", p_open, ImGuiWindowFlags_NoCollapse))
        {
            ImGui::End();
            return;
        }

        const auto &allWs = vtServer->get_all_ws();
        auto activeWs = vtServer->get_active_ws();

        ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.0f, 1.0f), "SYSTEM MULTI-WORKING SET (ISO 11783)");
        ImGui::TextDisabled("Rownoczesny podglad maszyn na magistrali CAN:");
        ImGui::Separator();
        ImGui::Spacing();

        if (allWs.empty())
        {
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Brak podlaczonych maszyn na magistrali CAN.");
            ImGui::TextDisabled("Podlacz maszyne (np. ESP32 seeder na magistrali).");
            ImGui::End();
            return;
        }

        for (std::size_t i = 0; i < allWs.size(); ++i)
        {
            auto currentWs = allWs[i];
            if (!currentWs) continue;

            bool isActive = (currentWs == activeWs);
            bool isAlarm = vtServer->is_alarm_active_on_ws(currentWs);
            std::uint8_t addr = currentWs->get_control_function() ? currentWs->get_control_function()->get_address() : 0xFE;

            ImGui::PushID(static_cast<int>(i));

            char headerTitle[96];
            std::snprintf(headerTitle, sizeof(headerTitle), "%s Maszyna #%zu (ECU 0x%02X) %s",
                          isActive ? "*" : " ",
                          i + 1, addr,
                          isActive ? "[AKTYWNA]" : (isAlarm ? "[! ALARM !]" : "[W TLE]"));

            ImVec4 headerCol = isAlarm ? ImVec4(0.85f, 0.20f, 0.15f, 1.0f) :
                               (isActive ? ImVec4(0.15f, 0.65f, 0.35f, 1.0f) : ImVec4(0.20f, 0.28f, 0.40f, 1.0f));

            ImGui::PushStyleColor(ImGuiCol_Header, headerCol);
            bool open = ImGui::CollapsingHeader(headerTitle, ImGuiTreeNodeFlags_DefaultOpen);
            ImGui::PopStyleColor();

            if (open)
            {
                if (isActive)
                {
                    ImGui::TextColored(ImVec4(0.25f, 0.90f, 0.45f, 1.0f), "[*] Wyswietlana na glownym terminalu");
                }
                else
                {
                    if (isAlarm)
                    {
                        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "[!] MASZYNA ZGLASZA ALARM!");
                    }
                    if (ImGui::Button(" Przepnij na glowny ekran ", ImVec2(-1, 28)))
                    {
                        AudioBeeper::get_instance().play_touch_click();
                        vtServer->switch_working_set(i);
                    }
                }

                std::uint16_t maskId = vtServer->get_working_set_active_mask(currentWs);
                float miniSize = 220.0f;
                ImVec2 canvasPos = ImGui::GetCursorScreenPos();
                ImDrawList *dl = ImGui::GetWindowDrawList();

                // Thumbnail canvas frame
                dl->AddRectFilled(canvasPos, ImVec2(canvasPos.x + miniSize, canvasPos.y + miniSize),
                                  IM_COL32(18, 22, 30, 255), 4.0f);
                dl->AddRect(canvasPos, ImVec2(canvasPos.x + miniSize, canvasPos.y + miniSize),
                            isActive ? IM_COL32(40, 180, 90, 255) : (isAlarm ? IM_COL32(220, 60, 40, 255) : IM_COL32(50, 65, 88, 255)),
                            4.0f, 0, isActive ? 2.0f : 1.0f);

                if (maskId != isobus::NULL_OBJECT_ID)
                {
                    render_data_mask(dl, canvasPos, ImVec2(miniSize, miniSize), currentWs, maskId);
                }
                else
                {
                    dl->AddText(ImVec2(canvasPos.x + 20, canvasPos.y + 100), IM_COL32(160, 170, 185, 255), "Ladowanie puli...");
                }

                // Invisible button overlay
                ImGui::InvisibleButton("##miniCanvas", ImVec2(miniSize, miniSize));
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Kliknij lewym przyciskiem myszy, aby przelaczyc na te maszyne");
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    {
                        AudioBeeper::get_instance().play_touch_click();
                        vtServer->switch_working_set(i);
                    }
                }

                ImGui::TextDisabled("Maska: #%u | Obiektow: %zu", maskId, currentWs->get_object_tree().size());
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::PopID();
        }

        ImGui::End();
    }

} // namespace isobus_vt
