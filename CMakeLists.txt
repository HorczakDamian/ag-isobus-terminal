cmake_minimum_required(VERSION 3.16)

project(
  isobus_precision_ag
  VERSION 1.0.0
  LANGUAGES CXX C
  DESCRIPTION "ISOBUS Universal Terminal (VT Server) & AgOpenGPS Task Controller Bridge"
)

# Build the main ISOBUS VT terminal and AgOpenGPS Task Controller bridge
add_subdirectory(isobus_vt_terminal)
