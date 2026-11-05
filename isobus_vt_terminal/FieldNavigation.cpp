#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "FieldNavigation.hpp"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <chrono>

namespace isobus_vt
{
    static constexpr double PI = 3.14159265358979323846;
    static constexpr double DEG_TO_RAD = PI / 180.0;
    static constexpr double RAD_TO_DEG = 180.0 / PI;
    static constexpr double EARTH_RADIUS_M = 6378137.0;

    static double normalize_angle_deg(double angle)
    {
        while (angle > 180.0) angle -= 360.0;
        while (angle < -180.0) angle += 360.0;
        return angle;
    }

    // =========================================================================
    // AbLineGuidance Implementation
    // =========================================================================
    AbLineGuidance::AbLineGuidance() = default;

    void AbLineGuidance::set_point_a(const Vector2D &pos, float headingDeg)
    {
        pointA = pos;
        pointAValid = true;
        pointBValid = false;
        abHeadingDeg = headingDeg;

        double rad = (90.0 - headingDeg) * DEG_TO_RAD;
        directionUnit = { std::cos(rad), std::sin(rad) };
        normalUnit = { -directionUnit.y, directionUnit.x };
    }

    void AbLineGuidance::set_point_b(const Vector2D &pos)
    {
        if (!pointAValid) return;

        pointB = pos;
        double dist = pointA.distance_to(pointB);
        if (dist > 2.0) // Minimum 2m separation
        {
            pointBValid = true;
            directionUnit = { (pointB.x - pointA.x) / dist, (pointB.y - pointA.y) / dist };
            normalUnit = { -directionUnit.y, directionUnit.x };

            double angleRad = std::atan2(directionUnit.y, directionUnit.x);
            abHeadingDeg = static_cast<float>(normalize_angle_deg(90.0 - angleRad * RAD_TO_DEG));
            if (abHeadingDeg < 0.0f) abHeadingDeg += 360.0f;
        }
    }

    void AbLineGuidance::reset()
    {
        pointAValid = false;
        pointBValid = false;
    }

    double AbLineGuidance::calculate_xte(const Vector2D &pos, double toolWidth, int &outSwathIndex, double &outDistanceAlongLine) const
    {
        if (!pointAValid)
        {
            outSwathIndex = 0;
            outDistanceAlongLine = 0.0;
            return 0.0;
        }

        Vector2D rel = pos - pointA;
        outDistanceAlongLine = rel.x * directionUnit.x + rel.y * directionUnit.y;
        double distPerp = rel.x * normalUnit.x + rel.y * normalUnit.y;

        if (toolWidth <= 0.1) toolWidth = 1.0;

        outSwathIndex = static_cast<int>(std::round(distPerp / toolWidth));
        double xte = distPerp - (outSwathIndex * toolWidth);
        return xte; // meters
    }

    void AbLineGuidance::get_swath_endpoints(int swathIndex, double toolWidth, double lengthMeters, Vector2D &outStart, Vector2D &outEnd) const
    {
        Vector2D swathOrigin = pointA + normalUnit * (swathIndex * toolWidth);
        outStart = swathOrigin - directionUnit * (lengthMeters * 0.5);
        outEnd = swathOrigin + directionUnit * (lengthMeters * 0.5);
    }

    // =========================================================================
    // AutoSteerController Implementation
    // =========================================================================
    AutoSteerController::AutoSteerController() = default;

    float AutoSteerController::compute_steer_angle(const Vector2D &tractorPos, float tractorHeadingDeg, float speedKph,
                                                  const AbLineGuidance &abGuidance, double toolWidth)
    {
        if (!engaged || !abGuidance.is_ab_valid())
        {
            targetSteerAngle = 0.0f;
            return 0.0f;
        }

        int swathIdx = 0;
        double alongDist = 0.0;
        double xte = abGuidance.calculate_xte(tractorPos, toolWidth, swathIdx, alongDist);

        double headingDiff = normalize_angle_deg(tractorHeadingDeg - abGuidance.get_ab_heading());

        // If driving opposite direction on swath, flip heading reference
        if (std::abs(headingDiff) > 90.0)
        {
            headingDiff = normalize_angle_deg(headingDiff - 180.0);
            xte = -xte; // invert XTE for opposite direction
        }

        double speedMps = std::max(0.5, (double)speedKph * (1000.0 / 3600.0));

        // Stanley steering algorithm: delta = heading_error + atan2(k * xte, v)
        double stanleyComponent = std::atan2(stanleyGain * xte, speedMps) * RAD_TO_DEG;
        double commanded = -headingDiff - stanleyComponent;

        commanded = std::clamp(commanded, -(double)maxSteerAngle, (double)maxSteerAngle);
        targetSteerAngle = static_cast<float>(commanded);
        return targetSteerAngle;
    }

    // =========================================================================
    // FieldCoverage Implementation
    // =========================================================================
    FieldCoverage::FieldCoverage()
    {
        configure_tool(12.0, 16);
    }

    void FieldCoverage::configure_tool(double totalWidth, uint8_t numSections)
    {
        std::lock_guard<std::recursive_mutex> lock(coverageMutex);
        toolWidth = totalWidth;
        if (numSections == 0) numSections = 1;

        sections.clear();
        sections.resize(numSections);
        double secWidth = totalWidth / numSections;

        for (uint8_t i = 0; i < numSections; ++i)
        {
            sections[i].index = i;
            sections[i].widthMeters = secWidth;
            sections[i].offsetLeftMeters = -totalWidth * 0.5 + i * secWidth;
            sections[i].offsetRightMeters = sections[i].offsetLeftMeters + secWidth;
            sections[i].active = true;
            sections[i].autoOff = false;
        }
    }

    void FieldCoverage::update_sections_from_tc(uint8_t numSections, const std::vector<bool> &states)
    {
        std::lock_guard<std::recursive_mutex> lock(coverageMutex);
        if (sections.size() != numSections)
        {
            configure_tool(toolWidth, numSections);
        }
        for (size_t i = 0; i < sections.size() && i < states.size(); ++i)
        {
            sections[i].active = states[i];
        }
    }

    void FieldCoverage::clear_coverage()
    {
        std::lock_guard<std::recursive_mutex> lock(coverageMutex);
        vertices.clear();
        hasLastRecord = false;
        totalAreaSqm = 0.0;
    }

    void FieldCoverage::record_step(const Vector2D &tractorPos, float headingDeg, double hitchOffset, bool isMasterOn)
    {
        std::lock_guard<std::recursive_mutex> lock(coverageMutex);

        if (!hasLastRecord)
        {
            lastRecordPos = tractorPos;
            hasLastRecord = true;
            return;
        }

        double dist = lastRecordPos.distance_to(tractorPos);
        if (dist < 0.25) return; // Record every 25 cm of forward movement

        double rad = (90.0 - headingDeg) * DEG_TO_RAD;
        Vector2D fwd{ std::cos(rad), std::sin(rad) };
        Vector2D right{ fwd.y, -fwd.x };

        Vector2D toolPos = tractorPos - fwd * hitchOffset;
        Vector2D prevToolPos = lastRecordPos - fwd * hitchOffset;

        if (isMasterOn)
        {
            for (const auto &sec : sections)
            {
                if (!sec.active || sec.autoOff) continue;

                Vector2D p1 = prevToolPos + right * sec.offsetLeftMeters;
                Vector2D p2 = prevToolPos + right * sec.offsetRightMeters;
                Vector2D p3 = toolPos + right * sec.offsetRightMeters;
                Vector2D p4 = toolPos + right * sec.offsetLeftMeters;

                // Triangle 1: p1, p2, p3
                vertices.push_back({ p1, 0x8828D858 });
                vertices.push_back({ p2, 0x8828D858 });
                vertices.push_back({ p3, 0x8828D858 });

                // Triangle 2: p1, p3, p4
                vertices.push_back({ p1, 0x8828D858 });
                vertices.push_back({ p3, 0x8828D858 });
                vertices.push_back({ p4, 0x8828D858 });

                totalAreaSqm += sec.widthMeters * dist;
            }
        }

        lastRecordPos = tractorPos;
    }

    // =========================================================================
    // FieldNavigation Implementation
    // =========================================================================
    FieldNavigation& FieldNavigation::get_instance()
    {
        static FieldNavigation instance;
        return instance;
    }

    FieldNavigation::FieldNavigation()
    {
        cameraCenter = { 0.0, 0.0 };
    }

    void FieldNavigation::set_field_origin(double lat, double lon)
    {
        std::lock_guard<std::recursive_mutex> lock(dataMutex);
        originLat = lat;
        originLon = lon;
        originSet = true;
    }

    void FieldNavigation::project_to_local_meters(double lat, double lon, Vector2D &outMeters) const
    {
        double dLat = (lat - originLat) * DEG_TO_RAD;
        double dLon = (lon - originLon) * DEG_TO_RAD;
        double meanLat = originLat * DEG_TO_RAD;

        outMeters.x = dLon * EARTH_RADIUS_M * std::cos(meanLat);
        outMeters.y = dLat * EARTH_RADIUS_M;
    }

    void FieldNavigation::update_gps_nmea(double lat, double lon, double alt, float speedKph, float headingDeg,
                                          GpsFixQuality fixQuality, uint8_t sats, float hdop)
    {
        std::lock_guard<std::recursive_mutex> lock(dataMutex);

        if (!originSet && (lat != 0.0 || lon != 0.0))
        {
            originLat = lat;
            originLon = lon;
            originSet = true;
        }

        telemetry.latitude = lat;
        telemetry.longitude = lon;
        telemetry.altitude = alt;
        telemetry.speedKph = speedKph;
        telemetry.headingDeg = headingDeg;
        telemetry.fixQuality = fixQuality;
        telemetry.satellites = sats;
        telemetry.hdop = hdop;
        telemetry.hasFix = (fixQuality != GpsFixQuality::Invalid);
        telemetry.lastUpdateMs = GetTickCount();

        if (originSet)
        {
            project_to_local_meters(lat, lon, telemetry.localPos);

            // Terrain roll compensation (antenna height = 2.8m)
            constexpr double ANTENNA_HEIGHT = 2.8;
            if (std::abs(telemetry.rollDeg) > 0.1f)
            {
                double rollRad = telemetry.rollDeg * DEG_TO_RAD;
                double headRad = (90.0 - telemetry.headingDeg) * DEG_TO_RAD;
                telemetry.localPos.x -= ANTENNA_HEIGHT * std::sin(rollRad) * std::cos(headRad);
                telemetry.localPos.y += ANTENNA_HEIGHT * std::sin(rollRad) * std::sin(headRad);
            }
        }

        if (!simulationMode)
        {
            coverage.record_step(telemetry.localPos, telemetry.headingDeg, 3.5, isMasterWorkSwitchOn);
            autoSteer.compute_steer_angle(telemetry.localPos, telemetry.headingDeg, telemetry.speedKph, abGuidance, coverage.get_tool_width());
        }
    }

    void FieldNavigation::update_imu(float rollDeg, float pitchDeg)
    {
        std::lock_guard<std::recursive_mutex> lock(dataMutex);
        telemetry.rollDeg = rollDeg;
        telemetry.pitchDeg = pitchDeg;
    }

    void FieldNavigation::update_steer_feedback(float actualSteerAngle, bool, bool)
    {
        std::lock_guard<std::recursive_mutex> lock(dataMutex);
        autoSteer.set_actual_steer_angle(actualSteerAngle);
    }

    GpsTelemetry FieldNavigation::get_telemetry() const
    {
        std::lock_guard<std::recursive_mutex> lock(dataMutex);
        return telemetry;
    }

    void FieldNavigation::drive_simulation_step(float steerInput, float speedInput, float dtSeconds)
    {
        std::lock_guard<std::recursive_mutex> lock(dataMutex);
        if (!simulationMode) return;

        simSpeedKph = std::clamp(simSpeedKph + speedInput * 2.0f, 0.0f, 30.0f);
        float speedMps = simSpeedKph * (1000.0f / 3600.0f);

        // Bicycle kinematic model
        constexpr float WHEELBASE = 2.85f;
        float actualAngle = steerInput;
        if (autoSteer.is_engaged())
        {
            actualAngle = autoSteer.compute_steer_angle(simTractorPos, simHeadingDeg, simSpeedKph, abGuidance, coverage.get_tool_width());
        }
        autoSteer.set_actual_steer_angle(actualAngle);

        float yawRateRad = (speedMps / WHEELBASE) * std::tan(actualAngle * (float)DEG_TO_RAD);
        simHeadingDeg += yawRateRad * dtSeconds * (float)RAD_TO_DEG;
        while (simHeadingDeg >= 360.0f) simHeadingDeg -= 360.0f;
        while (simHeadingDeg < 0.0f) simHeadingDeg += 360.0f;

        float rad = (90.0f - simHeadingDeg) * (float)DEG_TO_RAD;
        simTractorPos.x += std::cos(rad) * speedMps * dtSeconds;
        simTractorPos.y += std::sin(rad) * speedMps * dtSeconds;

        telemetry.localPos = simTractorPos;
        telemetry.headingDeg = simHeadingDeg;
        telemetry.speedKph = simSpeedKph;
        telemetry.fixQuality = GpsFixQuality::RtkFix;
        telemetry.satellites = 24;
        telemetry.hdop = 0.6f;
        telemetry.hasFix = true;
        telemetry.lastUpdateMs = GetTickCount();

        coverage.record_step(simTractorPos, simHeadingDeg, 3.5, isMasterWorkSwitchOn);
    }

    void FieldNavigation::render_lightbar_hud(float width)
    {
        Vector2D tractorPos = simulationMode ? simTractorPos : telemetry.localPos;
        int swathIdx = 0;
        double alongDist = 0.0;
        double xteMeters = abGuidance.calculate_xte(tractorPos, coverage.get_tool_width(), swathIdx, alongDist);
        double xteCm = xteMeters * 100.0;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.10f, 0.12f, 0.85f));
        ImGui::BeginChild("LightbarHUD", ImVec2(width, 42.0f), true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        float midX = p0.x + width * 0.5f;
        float midY = p0.y + 12.0f;

        // Center target bracket
        dl->AddLine(ImVec2(midX, midY - 10), ImVec2(midX, midY + 10), IM_COL32(255, 255, 255, 200), 2.0f);
        dl->AddRect(ImVec2(midX - 12, midY - 8), ImVec2(midX + 12, midY + 8), IM_COL32(40, 240, 60, 220), 2.0f);

        // LED blocks (10 on each side, 5cm per block)
        constexpr int NUM_LEDS = 10;
        constexpr float LED_SPACING = 14.0f;
        constexpr float LED_W = 10.0f;
        constexpr float LED_H = 12.0f;

        int activeLed = static_cast<int>(std::round(xteCm / 4.0)); // 4cm per LED

        for (int i = 1; i <= NUM_LEDS; ++i)
        {
            // Left LEDs (steer right)
            float lx = midX - i * LED_SPACING - 12.0f;
            uint32_t colL = (activeLed < 0 && std::abs(activeLed) >= i) ?
                ((i > 6) ? IM_COL32(240, 40, 40, 255) : IM_COL32(240, 200, 30, 255)) :
                IM_COL32(45, 55, 65, 120);
            dl->AddRectFilled(ImVec2(lx, midY - LED_H * 0.5f), ImVec2(lx + LED_W, midY + LED_H * 0.5f), colL, 2.0f);

            // Right LEDs (steer left)
            float rx = midX + i * LED_SPACING + 2.0f;
            uint32_t colR = (activeLed > 0 && activeLed >= i) ?
                ((i > 6) ? IM_COL32(240, 40, 40, 255) : IM_COL32(240, 200, 30, 255)) :
                IM_COL32(45, 55, 65, 120);
            dl->AddRectFilled(ImVec2(rx, midY - LED_H * 0.5f), ImVec2(rx + LED_W, midY + LED_H * 0.5f), colR, 2.0f);
        }

        // Numerical XTE readout
        char xteBuf[64];
        if (abGuidance.is_ab_valid())
        {
            if (std::abs(xteCm) < 2.0)
                snprintf(xteBuf, sizeof(xteBuf), "IDELANIE (0 cm)");
            else if (xteCm > 0.0)
                snprintf(xteBuf, sizeof(xteBuf), "<-- SKREC W LEWO: %.1f cm", xteCm);
            else
                snprintf(xteBuf, sizeof(xteBuf), "SKREC W PRAWO: %.1f cm -->", std::abs(xteCm));
        }
        else
        {
            snprintf(xteBuf, sizeof(xteBuf), "[ BRAK LINII AB - USTAWIENIE A / B ]");
        }

        ImVec2 textSz = ImGui::CalcTextSize(xteBuf);
        dl->AddText(ImVec2(midX - textSz.x * 0.5f, midY + 9.0f),
                    (std::abs(xteCm) < 3.0) ? IM_COL32(40, 255, 60, 255) : IM_COL32(255, 230, 100, 255),
                    xteBuf);

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void FieldNavigation::render_navigation_viewport(const char *windowId, const ImVec2 &size, bool embeddedMode)
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild(windowId, size, true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 canvasP0 = ImGui::GetCursorScreenPos();
        ImVec2 canvasSz = ImGui::GetContentRegionAvail();
        if (canvasSz.x < 50.0f) canvasSz.x = 50.0f;
        if (canvasSz.y < 50.0f) canvasSz.y = 50.0f;
        ImVec2 canvasP1 = ImVec2(canvasP0.x + canvasSz.x, canvasP0.y + canvasSz.y);

        // Clip to canvas area
        dl->PushClipRect(canvasP0, canvasP1, true);

        // Background: Field Soil / Satellite Dark Green
        dl->AddRectFilled(canvasP0, canvasP1, IM_COL32(20, 24, 28, 255));

        Vector2D tractorPos = simulationMode ? simTractorPos : telemetry.localPos;
        float tractorHeading = simulationMode ? simHeadingDeg : telemetry.headingDeg;

        // Camera follow modes
        if (cameraMode == CameraMode::FollowTractorChase || cameraMode == CameraMode::FollowTractorNorthUp)
        {
            cameraCenter = tractorPos;
        }

        // Handle Pan & Zoom via mouse dragging
        if (ImGui::IsWindowHovered())
        {
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f)
            {
                zoomPixelsPerMeter = std::clamp(zoomPixelsPerMeter * (1.0f + wheel * 0.15f), 1.5f, 80.0f);
            }

            if (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && cameraMode == CameraMode::FreePanZoom))
            {
                ImVec2 delta = ImGui::GetIO().MouseDelta;
                cameraCenter.x -= delta.x / zoomPixelsPerMeter;
                cameraCenter.y += delta.y / zoomPixelsPerMeter;
                cameraMode = CameraMode::FreePanZoom;
            }
        }

        ImVec2 screenCenter = ImVec2(canvasP0.x + canvasSz.x * 0.5f, canvasP0.y + canvasSz.y * 0.5f);
        float cameraRotationRad = (cameraMode == CameraMode::FollowTractorChase) ? (float)(tractorHeading * DEG_TO_RAD) : 0.0f;
        float cosCam = std::cos(-cameraRotationRad);
        float sinCam = std::sin(-cameraRotationRad);

        // Transformation helper: World meters -> Canvas screen pixels
        auto WorldToScreen = [&](const Vector2D &world) -> ImVec2
        {
            double dx = world.x - cameraCenter.x;
            double dy = world.y - cameraCenter.y;
            double rx = dx * cosCam - dy * sinCam;
            double ry = dx * sinCam + dy * cosCam;
            return ImVec2(screenCenter.x + (float)(rx * zoomPixelsPerMeter),
                          screenCenter.y - (float)(ry * zoomPixelsPerMeter));
        };

        // 1. Grid lines (every 20m and 100m)
        double gridStep = (zoomPixelsPerMeter > 20.0f) ? 10.0 : ((zoomPixelsPerMeter > 6.0f) ? 25.0 : 100.0);
        int minGridX = static_cast<int>(std::floor((cameraCenter.x - canvasSz.x / zoomPixelsPerMeter) / gridStep));
        int maxGridX = static_cast<int>(std::ceil((cameraCenter.x + canvasSz.x / zoomPixelsPerMeter) / gridStep));
        int minGridY = static_cast<int>(std::floor((cameraCenter.y - canvasSz.y / zoomPixelsPerMeter) / gridStep));
        int maxGridY = static_cast<int>(std::ceil((cameraCenter.y + canvasSz.y / zoomPixelsPerMeter) / gridStep));

        for (int gx = minGridX; gx <= maxGridX; ++gx)
        {
            Vector2D gA{ gx * gridStep, minGridY * gridStep };
            Vector2D gB{ gx * gridStep, maxGridY * gridStep };
            dl->AddLine(WorldToScreen(gA), WorldToScreen(gB), IM_COL32(40, 50, 60, 90), 1.0f);
        }
        for (int gy = minGridY; gy <= maxGridY; ++gy)
        {
            Vector2D gA{ minGridX * gridStep, gy * gridStep };
            Vector2D gB{ maxGridX * gridStep, gy * gridStep };
            dl->AddLine(WorldToScreen(gA), WorldToScreen(gB), IM_COL32(40, 50, 60, 90), 1.0f);
        }

        // 2. Render Painted Field Coverage (Triangle Mesh)
        const auto &covVerts = coverage.get_vertices();
        for (size_t i = 0; i + 2 < covVerts.size(); i += 3)
        {
            ImVec2 sp1 = WorldToScreen(covVerts[i].pos);
            ImVec2 sp2 = WorldToScreen(covVerts[i + 1].pos);
            ImVec2 sp3 = WorldToScreen(covVerts[i + 2].pos);
            dl->AddTriangleFilled(sp1, sp2, sp3, covVerts[i].color);
        }

        // 3. Render AB Line & Parallel Swaths
        if (abGuidance.is_point_a_set())
        {
            ImVec2 spA = WorldToScreen(abGuidance.get_point_a());
            dl->AddCircleFilled(spA, 7.0f, IM_COL32(255, 50, 50, 255));
            dl->AddText(ImVec2(spA.x + 9, spA.y - 8), IM_COL32(255, 100, 100, 255), "PUNKT A");
        }

        if (abGuidance.is_ab_valid())
        {
            ImVec2 spB = WorldToScreen(abGuidance.get_point_b());
            dl->AddCircleFilled(spB, 7.0f, IM_COL32(50, 255, 50, 255));
            dl->AddText(ImVec2(spB.x + 9, spB.y - 8), IM_COL32(100, 255, 100, 255), "PUNKT B");

            // Find current swath index
            int currentSwath = 0;
            double along = 0.0;
            abGuidance.calculate_xte(tractorPos, coverage.get_tool_width(), currentSwath, along);

            // Render swaths around tractor (-6 to +6)
            for (int s = currentSwath - 8; s <= currentSwath + 8; ++s)
            {
                Vector2D startPt, endPt;
                abGuidance.get_swath_endpoints(s, coverage.get_tool_width(), 1000.0, startPt, endPt);
                ImVec2 s0 = WorldToScreen(startPt);
                ImVec2 s1 = WorldToScreen(endPt);

                if (s == currentSwath)
                {
                    // Active swath: bright cyan
                    dl->AddLine(s0, s1, IM_COL32(0, 240, 255, 240), 2.5f);
                }
                else
                {
                    // Adjacent swaths: dashed / soft blue
                    dl->AddLine(s0, s1, IM_COL32(40, 130, 200, 100), 1.2f);
                }
            }
        }

        // 4. Render Implement & Sections behind tractor
        double toolWidth = coverage.get_tool_width();
        const auto &sections = coverage.get_sections();
        double radTractor = (90.0 - tractorHeading) * DEG_TO_RAD;
        Vector2D fwdTractor{ std::cos(radTractor), std::sin(radTractor) };
        Vector2D rightTractor{ fwdTractor.y, -fwdTractor.x };
        Vector2D hitchPos = tractorPos - fwdTractor * 3.5; // 3.5m back

        // Drawbar line
        dl->AddLine(WorldToScreen(tractorPos), WorldToScreen(hitchPos), IM_COL32(180, 180, 180, 220), 3.0f);

        // Boom bar
        Vector2D boomL = hitchPos - rightTractor * (toolWidth * 0.5);
        Vector2D boomR = hitchPos + rightTractor * (toolWidth * 0.5);
        dl->AddLine(WorldToScreen(boomL), WorldToScreen(boomR), IM_COL32(230, 230, 230, 255), 4.0f);

        // Sections
        for (const auto &sec : sections)
        {
            Vector2D sL = hitchPos + rightTractor * sec.offsetLeftMeters;
            Vector2D sR = hitchPos + rightTractor * sec.offsetRightMeters;
            ImVec2 pL = WorldToScreen(sL);
            ImVec2 pR = WorldToScreen(sR);

            uint32_t secCol = sec.autoOff ? IM_COL32(240, 200, 30, 255) :
                              (sec.active ? IM_COL32(40, 240, 70, 255) : IM_COL32(110, 110, 110, 200));

            dl->AddLine(pL, pR, secCol, 5.0f);
            dl->AddCircleFilled(pL, 2.5f, IM_COL32(255, 255, 255, 255));
        }

        // 5. Render Tractor (Realistic Chassis, Cab, Wheels)
        ImVec2 tractorScr = WorldToScreen(tractorPos);
        float wheelAngleRad = autoSteer.get_actual_steer_angle() * (float)DEG_TO_RAD;
        float bodyAngleRad = (float)radTractor;

        // Front and rear axle world points
        Vector2D frontAxle = tractorPos + fwdTractor * 1.5;
        Vector2D rearAxle = tractorPos - fwdTractor * 1.35;
        float trackWidthHalf = 1.05f;

        // Wheel rendering helper
        auto DrawWheel = [&](const Vector2D &hub, float rotRad, float len, float thickness, uint32_t col)
        {
            Vector2D wFwd{ std::cos(rotRad), std::sin(rotRad) };
            Vector2D wStart = hub - wFwd * (len * 0.5);
            Vector2D wEnd = hub + wFwd * (len * 0.5);
            dl->AddLine(WorldToScreen(wStart), WorldToScreen(wEnd), col, thickness);
        };

        // Rear Wheels (drive wheels, bigger)
        DrawWheel(rearAxle - rightTractor * trackWidthHalf, bodyAngleRad, 1.6f, 6.0f, IM_COL32(25, 25, 25, 255));
        DrawWheel(rearAxle + rightTractor * trackWidthHalf, bodyAngleRad, 1.6f, 6.0f, IM_COL32(25, 25, 25, 255));

        // Front Wheels (steerable, turning with actual steer angle!)
        float frontWheelHeading = bodyAngleRad + wheelAngleRad;
        DrawWheel(frontAxle - rightTractor * trackWidthHalf, frontWheelHeading, 1.2f, 5.0f, IM_COL32(25, 25, 25, 255));
        DrawWheel(frontAxle + rightTractor * trackWidthHalf, frontWheelHeading, 1.2f, 5.0f, IM_COL32(25, 25, 25, 255));

        // Tractor Hood & Cab (Green Agri-Chassis)
        Vector2D n1 = frontAxle + fwdTractor * 0.6 - rightTractor * 0.55;
        Vector2D n2 = frontAxle + fwdTractor * 0.6 + rightTractor * 0.55;
        Vector2D n3 = rearAxle - fwdTractor * 0.4 + rightTractor * 0.75;
        Vector2D n4 = rearAxle - fwdTractor * 0.4 - rightTractor * 0.75;

        dl->AddQuadFilled(WorldToScreen(n1), WorldToScreen(n2), WorldToScreen(n3), WorldToScreen(n4), IM_COL32(40, 160, 45, 240));
        dl->AddQuad(WorldToScreen(n1), WorldToScreen(n2), WorldToScreen(n3), WorldToScreen(n4), IM_COL32(255, 255, 255, 180), 2.0f);

        // Glass Cab & GPS Dome
        Vector2D cabPos = tractorPos - fwdTractor * 0.2;
        dl->AddCircleFilled(WorldToScreen(cabPos), 8.0f, IM_COL32(220, 240, 255, 230));
        dl->AddCircle(WorldToScreen(cabPos), 8.0f, IM_COL32(20, 40, 60, 255), 0, 1.5f);
        // Yellow StarFire / GNSS dome on roof
        dl->AddCircleFilled(WorldToScreen(cabPos), 3.5f, IM_COL32(255, 220, 0, 255));

        // Heading direction pointer arrow
        Vector2D arrowTip = tractorPos + fwdTractor * 3.8;
        dl->AddLine(WorldToScreen(tractorPos), WorldToScreen(arrowTip), IM_COL32(255, 255, 255, 240), 2.0f);

        dl->PopClipRect();

        // =====================================================================
        // HUD OVERLAYS (Lightbar, Speed, AutoSteer Wheel, Buttons)
        // =====================================================================
        ImGui::SetCursorPos(ImVec2(10.0f, 10.0f));

        // Top Lightbar
        render_lightbar_hud(canvasSz.x - 20.0f);

        // Left HUD telemetry panel
        ImGui::SetCursorPos(ImVec2(15.0f, 65.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.10f, 0.12f, 0.88f));
        ImGui::BeginChild("NavTelemetry", ImVec2(240.0f, 195.0f), true);

        ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "TELEMETRIA NAWIGACJI");
        ImGui::Separator();

        const char *fixStr = (telemetry.fixQuality == GpsFixQuality::RtkFix) ? "RTK FIX (1-2 cm)" :
                             (telemetry.fixQuality == GpsFixQuality::RtkFloat) ? "RTK FLOAT (10 cm)" :
                             (telemetry.fixQuality == GpsFixQuality::Dgps) ? "DGPS (30 cm)" :
                             (telemetry.fixQuality == GpsFixQuality::GpsSingle) ? "GPS 3D (1.5 m)" : "BRAK FIXA";
        ImVec4 fixCol = (telemetry.fixQuality == GpsFixQuality::RtkFix) ? ImVec4(0.2f, 1.0f, 0.3f, 1.0f) :
                        (telemetry.fixQuality == GpsFixQuality::RtkFloat) ? ImVec4(1.0f, 0.9f, 0.2f, 1.0f) :
                        ImVec4(1.0f, 0.3f, 0.3f, 1.0f);

        ImGui::Text("Status GPS:");
        ImGui::SameLine();
        ImGui::TextColored(fixCol, "%s", fixStr);

        ImGui::Text("Satelity / HDOP: %u / %.1f", telemetry.satellites, telemetry.hdop);
        ImGui::Text("Predkosc:        %.1f km/h", telemetry.speedKph);
        ImGui::Text("Kurs (Heading):  %.1f deg", tractorHeading);
        ImGui::Text("Kat kol (Steer): %.1f deg", autoSteer.get_actual_steer_angle());
        ImGui::Text("Przechyl (Roll): %.1f deg", telemetry.rollDeg);
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.5f, 1.0f), "Obrobione: %.2f ha (%.0f m2)",
                           coverage.get_worked_area_ha(), coverage.get_worked_area_sqm());

        ImGui::EndChild();
        ImGui::PopStyleColor();

        // Right HUD: Big AutoSteer Wheel Engage Button
        ImGui::SetCursorPos(ImVec2(canvasSz.x - 110.0f, 65.0f));
        bool isEngaged = autoSteer.is_engaged();
        bool canEngage = abGuidance.is_ab_valid() && (telemetry.hasFix || simulationMode);

        ImVec4 steerBtnCol = isEngaged ? ImVec4(0.10f, 0.85f, 0.25f, 1.0f) :
                             (canEngage ? ImVec4(0.90f, 0.70f, 0.15f, 1.0f) : ImVec4(0.35f, 0.40f, 0.45f, 1.0f));

        ImGui::PushStyleColor(ImGuiCol_Button, steerBtnCol);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(steerBtnCol.x * 1.15f, steerBtnCol.y * 1.15f, steerBtnCol.z * 1.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.1f, 0.6f, 0.2f, 1.0f));

        if (ImGui::Button(isEngaged ? "AUTOSTEER\n[ AKTYWNY ]" : "AUTOSTEER\n[ GOTOWY ]", ImVec2(95.0f, 75.0f)))
        {
            if (canEngage || isEngaged)
            {
                autoSteer.toggle_engaged();
            }
        }
        ImGui::PopStyleColor(3);

        // Bottom Bar: AB Line Buttons & View Mode Controls
        ImGui::SetCursorPos(ImVec2(15.0f, canvasSz.y - 48.0f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.10f, 0.12f, 0.88f));
        ImGui::BeginChild("BottomControls", ImVec2(canvasSz.x - 30.0f, 40.0f), true);

        if (ImGui::Button(" [ A ] Punkt A ", ImVec2(110, 24)))
        {
            abGuidance.set_point_a(tractorPos, tractorHeading);
        }
        ImGui::SameLine();
        if (ImGui::Button(" [ B ] Punkt B ", ImVec2(110, 24)))
        {
            abGuidance.set_point_b(tractorPos);
        }
        ImGui::SameLine();
        if (ImGui::Button(" Reset AB ", ImVec2(90, 24)))
        {
            abGuidance.reset();
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        if (ImGui::Button(cameraMode == CameraMode::FollowTractorChase ? "Kamera: Sledz Traktor" :
                         (cameraMode == CameraMode::FollowTractorNorthUp ? "Kamera: Polnoc" : "Kamera: Swobodna"),
                         ImVec2(160, 24)))
        {
            cameraMode = (cameraMode == CameraMode::FollowTractorChase) ? CameraMode::FollowTractorNorthUp :
                         (cameraMode == CameraMode::FollowTractorNorthUp ? CameraMode::FreePanZoom : CameraMode::FollowTractorChase);
        }

        ImGui::SameLine();
        if (ImGui::Button("Wyczysc Pole", ImVec2(100, 24)))
        {
            coverage.clear_coverage();
        }

        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();

        bool sim = simulationMode;
        if (ImGui::Checkbox("Symulator Polowy (Test bez GPS)", &sim))
        {
            simulationMode = sim;
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();

        // Bench simulator drive controls (visible when simulator active)
        if (simulationMode)
        {
            ImGui::SetCursorPos(ImVec2(15.0f, canvasSz.y - 120.0f));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.14f, 0.18f, 0.92f));
            ImGui::BeginChild("SimControls", ImVec2(400.0f, 65.0f), true);

            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "STEROWANIE SYMULATOREM W BIURZE:");
            static float manualSteer = 0.0f;
            static float manualSpeed = 10.0f;
            ImGui::SetNextItemWidth(140);
            ImGui::SliderFloat("Kierownica", &manualSteer, -35.0f, 35.0f, "%.1f deg");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110);
            ImGui::SliderFloat("Gaz", &manualSpeed, 0.0f, 25.0f, "%.0f km/h");
            ImGui::SameLine();
            if (ImGui::Button("Zero")) { manualSteer = 0.0f; }

            // Drive simulation at 60 FPS
            float dt = ImGui::GetIO().DeltaTime;
            simSpeedKph = manualSpeed;
            drive_simulation_step(manualSteer, 0.0f, dt);

            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
}
