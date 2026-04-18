# obs_license_plate_blur — OBS async video filter: license plate detection + blur (YOLOv8 ONNX + OpenCV DNN)
#
# OBS compatibility: compile and link against the libobs headers/libs from the same OBS
# major.minor (e.g. 32.0) you target. OBS 32+ rejects plugins built with a newer libobs than the app.
#
# Prerequisites (Windows):
#   - CMake 3.16+, MSVC x64
#   - libobs (obs.h, obs-module.h, obs.lib) from your OBS version's development files
#   - OpenCV 4.x with dnn, imgproc, core
#
# Configure example (PowerShell):
#   cmake -B build -A x64 `
#     -D CMAKE_PREFIX_PATH="C:/path/to/opencv/build" `
#     -D LIBOBS_INCLUDE_DIR="C:/path/to/libobs" `
#     -D LIBOBS_LIB="C:/path/to/obs.lib"
#   cmake --build build --config Release
#
# Install (after Release build), example:
#   cmake --install build --prefix "C:/Program Files/obs-studio"
# Or copy manually:
#   build/Release/obs_license_plate_blur.dll -> <OBS>/obs-plugins/64bit/
#   data/obs-plugins/obs_license_plate_blur/ -> <OBS>/data/obs-plugins/obs_license_plate_blur/
#

cmake_minimum_required(VERSION 3.16)
project(obs_license_plate_blur VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W3 /EHsc)
endif()

set(LIBOBS_INCLUDE_DIR "" CACHE PATH "Directory containing obs.h (libobs)")
set(LIBOBS_LIB "" CACHE FILEPATH "Path to obs.lib")

if(NOT LIBOBS_INCLUDE_DIR OR NOT LIBOBS_LIB)
  message(FATAL_ERROR "Set LIBOBS_INCLUDE_DIR and LIBOBS_LIB (see comments at top of CMakeLists.txt).")
endif()

find_package(OpenCV REQUIRED COMPONENTS core imgproc dnn)

add_library(obs_license_plate_blur MODULE
  src/plugin-main.cpp
  src/detect-filter.cpp
)

target_include_directories(obs_license_plate_blur PRIVATE
  "${LIBOBS_INCLUDE_DIR}"
)

target_link_libraries(obs_license_plate_blur PRIVATE
  "${LIBOBS_LIB}"
  opencv_core
  opencv_imgproc
  opencv_dnn
)

if(WIN32)
  set_target_properties(obs_license_plate_blur PROPERTIES PREFIX "")
endif()

target_compile_definitions(obs_license_plate_blur PRIVATE
  $<$<PLATFORM_ID:Windows>:NOMINMAX>
)

install(TARGETS obs_license_plate_blur
  RUNTIME DESTINATION obs-plugins/64bit
  LIBRARY DESTINATION obs-plugins/64bit)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/data/obs-plugins/obs_license_plate_blur/"
  DESTINATION "data/obs-plugins/obs_license_plate_blur")
