cmake_minimum_required(VERSION 3.16)

project(
  isobus_vt_terminal
  VERSION 1.0.0
  LANGUAGES CXX C
  DESCRIPTION "ISOBUS Universal Terminal (VT Server) with AUX-N using ImGui and PEAK PCAN"
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(ISOBUS_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../isobus_sdk")

# Precompiled ISOBUS libraries
add_library(Isobus STATIC IMPORTED GLOBAL)
set_target_properties(Isobus PROPERTIES
  IMPORTED_LOCATION "${ISOBUS_SDK_DIR}/lib/Isobus.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${ISOBUS_SDK_DIR}/include"
)

add_library(HardwareIntegration STATIC IMPORTED GLOBAL)
set_target_properties(HardwareIntegration PROPERTIES
  IMPORTED_LOCATION "${ISOBUS_SDK_DIR}/lib/HardwareIntegration.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${ISOBUS_SDK_DIR}/include"
)

add_library(Utility STATIC IMPORTED GLOBAL)
set_target_properties(Utility PROPERTIES
  IMPORTED_LOCATION "${ISOBUS_SDK_DIR}/lib/Utility.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${ISOBUS_SDK_DIR}/include"
)

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
  "FieldNavigation.hpp"
  "FieldNavigation.cpp"
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
  "TractorEcuManager.hpp"
  "TractorEcuManager.cpp"
  "TcBasReportManager.hpp"
  "TcBasReportManager.cpp"
  "IsobusFileServer.hpp"
  "IsobusFileServer.cpp"
  "DdiExplorerManager.hpp"
  "DdiExplorerManager.cpp"
  "ObjectPoolInspector.hpp"
  "ObjectPoolInspector.cpp"
  "AefPoolLinter.hpp"
  "AefPoolLinter.cpp" 
  "main.cpp"
  ${IMGUI_SOURCES}
)

add_executable(isobus_vt_terminal WIN32 ${VT_TERMINAL_SOURCES})

target_include_directories(isobus_vt_terminal PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${IMGUI_DIR}"
  "${IMGUI_DIR}/backends"
  "${ISOBUS_SDK_DIR}/include"
)

target_link_libraries(isobus_vt_terminal PRIVATE
  Isobus
  HardwareIntegration
  Utility
  "${ISOBUS_SDK_DIR}/lib/PCANBasic_x64.lib"
  d3d11
  d3dcompiler
  dxgi
  dbghelp
  winmm
  gdiplus
  shell32
)

if(MSVC)
  target_compile_options(isobus_vt_terminal PRIVATE /W3 /utf-8 /Zi)
  target_link_options(isobus_vt_terminal PRIVATE /DEBUG)
endif()

add_custom_command(TARGET isobus_vt_terminal POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/tractor_icons"
    "$<TARGET_FILE_DIR:isobus_vt_terminal>/tractor_icons"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/sor_database.json"
    "$<TARGET_FILE_DIR:isobus_vt_terminal>/sor_database.json"
  COMMENT "Copying tractor icons and sor_database.json to output directory"
)

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
