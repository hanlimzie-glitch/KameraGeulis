cmake_minimum_required(VERSION 3.21)

project(KameraGeulis
        VERSION 0.2.0
        DESCRIPTION "Native webcam beauty/makeup app for Windows (C++17 + OpenGL)"
        LANGUAGES CXX C)

# Windows/GPUPixel only supports Release. VS generators are multi-config but
# GPUPixel still requires CMAKE_BUILD_TYPE=Release to be set explicitly.
if(WIN32 AND NOT DEFINED CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
message(STATUS "KameraGeulis ${PROJECT_VERSION} — CMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
if(MSVC)
  add_compile_options(/W4 /permissive- /Zc:__cplusplus)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra)
endif()

option(KG_BUILD_TESTS "Build kg_core unit tests (Catch2)" ON)
option(KG_BUILD_APP "Build the Windows desktop app (kg_app)" ${WIN32})
option(KG_BUILD_GPUPIXEL_PIPELINE "Build the GPUPixel pipeline (Windows/macOS only)" ${WIN32})

include(cmake/Dependencies.cmake)

# ---------------------------------------------------------------------------
# kg_core — pure logic, no GL/OS dependencies; unit-tested on Linux + Windows
# ---------------------------------------------------------------------------
add_library(kg_core STATIC
  src/kg_core/src/one_euro.cc
  src/kg_core/src/landmark.cc
  src/kg_core/src/strings.cc
  src/kg_core/src/settings.cc)
target_include_directories(kg_core PUBLIC src/kg_core/include)
target_link_libraries(kg_core PUBLIC nlohmann_json::nlohmann_json)

# ---------------------------------------------------------------------------
# kg_tests
# ---------------------------------------------------------------------------
if(KG_BUILD_TESTS)
  enable_testing()
  add_executable(kg_tests
    src/kg_tests/bounded_queue_test.cc
    src/kg_tests/one_euro_test.cc
    src/kg_tests/landmark_test.cc
    src/kg_tests/strings_test.cc
    src/kg_tests/settings_test.cc)
  target_link_libraries(kg_tests PRIVATE kg_core Catch2::Catch2WithMain)
  add_test(NAME kg_tests COMMAND kg_tests)
endif()

# ---------------------------------------------------------------------------
# Windows-only: capture, pipeline, vcam, app
# ---------------------------------------------------------------------------
if(WIN32)
  # Capture (Media Foundation)
  add_library(kg_capture STATIC src/kg_capture/camera_capture.cc)
  target_include_directories(kg_capture PUBLIC src/kg_capture)
  target_link_libraries(kg_capture PUBLIC kg_core mfplat mf mfreadwrite mfuuid ole32 propsys)

  # Virtual camera sender (softcam via dynamic LoadLibrary — no link-time dep;
  # if softcam.dll/registration is missing at runtime, vcam reports unavailable)
  add_library(kg_vcam STATIC src/kg_vcam/softcam_sender.cc)
  target_link_libraries(kg_vcam PUBLIC kg_core)

  # Pipeline (GPUPixel wrapper). The landmark provider is an interface; the
  # ONNX Runtime provider lands in M1b. Null provider = landmark effects off.
  if(KG_BUILD_GPUPIXEL_PIPELINE)
    add_library(kg_pipeline STATIC
      src/kg_pipeline/face_tracker.cc
      src/kg_pipeline/beauty_pipeline.cc)
    target_include_directories(kg_pipeline PUBLIC src/kg_pipeline)
    target_link_libraries(kg_pipeline PUBLIC
      kg_core gpupixel glad glfw)
  endif()

  if(KG_BUILD_APP)
    add_executable(KameraGeulis WIN32
      src/kg_app/main.cc
      src/kg_app/panel.cc)
    target_include_directories(KameraGeulis PRIVATE src/kg_app)
    target_link_libraries(KameraGeulis PRIVATE
      kg_core kg_capture kg_vcam kg_pipeline
      imgui_kg glad glfw opengl32 gdi32)
  endif()
endif()
