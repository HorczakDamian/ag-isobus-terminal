cmake_minimum_required(VERSION 3.16)

project(
  isobus_vt_terminal
  VERSION 1.0.0
  LANGUAGES CXX C
  DESCRIPTION "ISOBUS Universal Terminal (VT Server) with AUX-N using ImGui and PEAK PCAN"
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable both Windows PCAN Basic and Virtual CAN
set(CAN_DRIVER "WindowsPCANBasic;VirtualCAN" CACHE STRING "CAN drivers to build" FORCE)

# Add AgIsoStack++ library (local folder if present, or auto-fetch via Git)
include(FetchContent)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../AgIsoStack-plus-plus-main/CMakeLists.txt")
  add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../AgIsoStack-plus-plus-main"
    "${CMAKE_CURRENT_BINARY_DIR}/ag_isostack"
  )
else()
  message(STATUS "AgIsoStack++ not found locally, fetching from GitHub...")
  FetchContent_Declare(
    agisostack
    GIT_REPOSITORY https://github.com/Open-Agriculture/AgIsoStack-plus-plus.git
    GIT_TAG main
  )
  FetchContent_MakeAvailable(agisostack)
endif()

# ImGui sources
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../imgui_src")
set(IMGUI_SOURCES
  "${IMGUI_DIR}/imgui.cpp"
  "${IMGUI_DIR}/imgui_draw.cpp"
  "${IMGUI_DIR}/imgui_tables.cpp"
  "${IMGUI_DIR}/imgui_widgets.cpp"
  "${IMGUI_DIR}/imgui_demo.cpp"
  "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
  "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp"
)

# Application Sources
set(VT_TERMINAL_SOURCES
  "AudioBeeper.hpp"
  "AudioBeeper.cpp"
  "TerminalSettings.hpp"
  "TerminalSettings.cpp"
  "SplashScreen.hpp"
  "SplashScreen.cpp"
  "CustomVTServer.hpp"
  "CustomVTServer.cpp"
  "AuxManager.hpp"
  "AuxManager.cpp"
  "CanManager.hpp"
  "CanManager.cpp"
  "TaskControllerManager.hpp"
  "TaskControllerManager.cpp"
  "VTRenderer.hpp"
  "VTRenderer.cpp"
  "main.cpp"
  ${IMGUI_SOURCES}
)

add_executable(isobus_vt_terminal WIN32 ${VT_TERMINAL_SOURCES})

target_include_directories(isobus_vt_terminal PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${IMGUI_DIR}"
  "${IMGUI_DIR}/backends"
  "${CMAKE_CURRENT_SOURCE_DIR}/../AgIsoStack-plus-plus-main/isobus/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../AgIsoStack-plus-plus-main/hardware_integration/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../AgIsoStack-plus-plus-main/utility/include"
)

target_link_libraries(isobus_vt_terminal PRIVATE
  Isobus
  HardwareIntegration
  Utility
  d3d11
  d3dcompiler
  dxgi
  dbghelp
)

if(MSVC)
  target_compile_options(isobus_vt_terminal PRIVATE /W3 /utf-8 /Zi)
  target_link_options(isobus_vt_terminal PRIVATE /DEBUG)
endif()

# AgOpenGPS Task Controller Bridge Launcher (replaces standalone AOG-TaskController.exe)
add_executable(AOG-TaskController "aog_bridge_main.cpp")
target_compile_features(AOG-TaskController PRIVATE cxx_std_17)
if(MSVC)
  target_compile_options(AOG-TaskController PRIVATE /W3 /utf-8)
endif()

add_custom_command(TARGET AOG-TaskController POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:AOG-TaskController>
    "${CMAKE_CURRENT_SOURCE_DIR}/../tcagopen/AOG-TaskController/bin/AOG-TaskController.exe"
  COMMENT "Deploying AOG-TaskController bridge to tcagopen/AOG-TaskController/bin"
)
