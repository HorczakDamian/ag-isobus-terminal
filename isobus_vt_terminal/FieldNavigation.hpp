#pragma once

#include <vector>
#include <string>
#include <cmath>
#include <memory>
#include <mutex>
#include <atomic>
#include <deque>
#include <functional>
#include "imgui.h"

namespace isobus_vt
{
    // GPS Fix Quality
    enum class GpsFixQuality : uint8_t
    {
        Invalid = 0,
        GpsSingle = 1,
        Dgps = 2,
        Pps = 3,
        RtkFix = 4,
        RtkFloat = 5,
        Estimated = 6,
        Manual = 7,
        Simulation = 8
    };

    // Point in 2D local Cartesian coordinates (meters)
    struct Vector2D
    {
        double x = 0.0; // Easting (meters)
        double y = 0.0; // Northing (meters)

        Vector2D() = default;
        Vector2D(double x, double y) : x(x), y(y) {}

        double distance_to(const Vector2D &other) const
        {
            double dx = x - other.x;
            double dy = y - other.y;
            return std::sqrt(dx * dx + dy * dy);
        }

        Vector2D operator+(const Vector2D &other) const { return { x + other.x, y + other.y }; }
        Vector2D operator-(const Vector2D &other) const { return { x - other.x, y - other.y }; }
        Vector2D operator*(double scalar) const { return { x * scalar, y * scalar }; }
    };

    // GPS Telemetry State
    struct GpsTelemetry
    {
        double latitude = 0.0;
        double longitude = 0.0;
        double altitude = 0.0;
        float speedKph = 0.0f;
        float headingDeg = 0.0f; // 0..360, North = 0, East = 90
        float rollDeg = 0.0f;
        float pitchDeg = 0.0f;
        GpsFixQuality fixQuality = GpsFixQuality::Invalid;
        uint8_t satellites = 0;
        float hdop = 99.9f;

        Vector2D localPos; // Local meters
        bool hasFix = false;
        uint32_t lastUpdateMs = 0;
    };

    // AB Reference Line and parallel swaths
    class AbLineGuidance
    {
    public:
        AbLineGuidance();

        void set_point_a(const Vector2D &pos, float headingDeg);
        void set_point_b(const Vector2D &pos);
        void reset();

        bool is_point_a_set() const { return pointAValid; }
        bool is_point_b_set() const { return pointBValid; }
        bool is_ab_valid() const { return pointAValid && pointBValid; }

        Vector2D get_point_a() const { return pointA; }
        Vector2D get_point_b() const { return pointB; }
        float get_ab_heading() const { return abHeadingDeg; }

        // Compute signed cross-track error (meters) to nearest swath. Positive = right of line
        double calculate_xte(const Vector2D &pos, double toolWidth, int &outSwathIndex, double &outDistanceAlongLine) const;

        // Get coordinates for rendering a given swath line index (A-B extended)
        void get_swath_endpoints(int swathIndex, double toolWidth, double lengthMeters, Vector2D &outStart, Vector2D &outEnd) const;

    private:
        Vector2D pointA;
        Vector2D pointB;
        bool pointAValid = false;
        bool pointBValid = false;
        float abHeadingDeg = 0.0f;
        Vector2D directionUnit;
        Vector2D normalUnit;
    };

    // Section Geometry & Lookahead
    struct SectionUnit
    {
        uint8_t index = 0;
        double widthMeters = 0.75;
        double offsetLeftMeters = 0.0; // from tool center
        double offsetRightMeters = 0.75;
        bool active = false;
        bool autoOff = false; // Triggered by overlap
    };

    // Real-time Coverage Mesh
    struct CoverageVertex
    {
        Vector2D pos;
        uint32_t color = 0xAA20E040; // Semi-transparent green
    };

    class FieldCoverage
    {
    public:
        FieldCoverage();

        void configure_tool(double totalWidth, uint8_t numSections);
        void update_sections_from_tc(uint8_t numSections, const std::vector<bool> &states);

        // Record driven path triangle strips
        void record_step(const Vector2D &tractorPos, float headingDeg, double hitchOffset, bool isMasterOn);

        void clear_coverage();
        double get_worked_area_sqm() const { return totalAreaSqm; }
        double get_worked_area_ha() const { return totalAreaSqm / 10000.0; }

        const std::vector<CoverageVertex>& get_vertices() const { return vertices; }
        const std::vector<SectionUnit>& get_sections() const { return sections; }
        double get_tool_width() const { return toolWidth; }

        // Check if section box overlaps existing coverage
        bool check_section_overlap(const Vector2D &secPos, double radiusMeters) const;

    private:
        double toolWidth = 12.0;
        std::vector<SectionUnit> sections;
        std::vector<CoverageVertex> vertices;
        Vector2D lastRecordPos;
        bool hasLastRecord = false;
        double totalAreaSqm = 0.0;
        mutable std::recursive_mutex coverageMutex;
    };

    // Camera view modes
    enum class CameraMode
    {
        FollowTractorChase = 0, // Rotates with tractor (chase view)
        FollowTractorNorthUp = 1, // Always North up, follows tractor
        FreePanZoom = 2
    };

    // AutoSteer Controller (Stanley / Pure Pursuit)
    class AutoSteerController
    {
    public:
        AutoSteerController();

        bool is_engaged() const { return engaged; }
        void set_engaged(bool eng) { engaged = eng; }
        void toggle_engaged() { engaged = !engaged; }

        // Computes target front wheel angle (degrees, negative = left, positive = right)
        float compute_steer_angle(const Vector2D &tractorPos, float tractorHeadingDeg, float speedKph,
                                  const AbLineGuidance &abGuidance, double toolWidth);

        float get_target_steer_angle() const { return targetSteerAngle; }
        float get_actual_steer_angle() const { return actualSteerAngle; }
        void set_actual_steer_angle(float angle) { actualSteerAngle = angle; }

        void set_max_steer_angle(float maxAngle) { maxSteerAngle = maxAngle; }
        void set_wheelbase(float wb) { wheelbase = wb; }
        void set_stanley_gain(float k) { stanleyGain = k; }

    private:
        bool engaged = false;
        float targetSteerAngle = 0.0f;
        float actualSteerAngle = 0.0f;
        float maxSteerAngle = 35.0f;
        float wheelbase = 2.85f; // Tractor wheelbase (meters)
        float stanleyGain = 1.2f; // Tuning gain
        float lookaheadGain = 1.0f;
    };

    // Central Field Navigation Coordinator & ImGui Canvas
    class FieldNavigation
    {
    public:
        static FieldNavigation& get_instance();

        // Feed GPS data parsed from AgIO / NMEA
        void update_gps_nmea(double lat, double lon, double alt, float speedKph, float headingDeg,
                             GpsFixQuality fixQuality, uint8_t sats, float hdop);

        void update_imu(float rollDeg, float pitchDeg);
        void update_steer_feedback(float actualSteerAngle, bool steerSwitch, bool workSwitch);

        // Core components access
        GpsTelemetry get_telemetry() const;
        AbLineGuidance& get_ab_guidance() { return abGuidance; }
        FieldCoverage& get_coverage() { return coverage; }
        AutoSteerController& get_autosteer() { return autoSteer; }

        // Simulation / Manual tractor driving for bench testing
        void set_simulation_mode(bool sim) { simulationMode = sim; }
        bool is_simulation_mode() const { return simulationMode; }
        void drive_simulation_step(float steerInput, float speedInput, float dtSeconds);

        // Render interactive Field Viewport in ImGui (DirectX 11)
        void render_navigation_viewport(const char *windowId, const ImVec2 &size, bool embeddedMode = false);

        // Render compact Lightbar HUD
        void render_lightbar_hud(float width);

        // Master Work Switch (np. z pozycji TUZ lub WOM ciągnika)
        void set_master_work_switch(bool workSwitch) { isMasterWorkSwitchOn = workSwitch; }
        bool get_master_work_switch() const { return isMasterWorkSwitchOn; }

        // Origin reference setting
        void set_field_origin(double lat, double lon);
        bool has_field_origin() const { return originSet; }

    private:
        FieldNavigation();
        ~FieldNavigation() = default;

        void project_to_local_meters(double lat, double lon, Vector2D &outMeters) const;

        mutable std::recursive_mutex dataMutex;
        GpsTelemetry telemetry;
        double originLat = 52.2297; // Default or first fix
        double originLon = 21.0122;
        bool originSet = false;
        bool isMasterWorkSwitchOn = true;

        AbLineGuidance abGuidance;
        FieldCoverage coverage;
        AutoSteerController autoSteer;

        // Viewport camera parameters
        CameraMode cameraMode = CameraMode::FollowTractorChase;
        Vector2D cameraCenter;
        float zoomPixelsPerMeter = 14.0f;
        bool isDragging = false;
        ImVec2 lastMousePos;

        // Simulation state
        bool simulationMode = false;
        Vector2D simTractorPos{ 0.0, 0.0 };
        float simHeadingDeg = 0.0f;
        float simSpeedKph = 8.0f;
    };
}
