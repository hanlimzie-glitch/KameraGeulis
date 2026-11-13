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
  src/kg_core/src/settings.cc
  src/kg_core/src/face_anchors.cc
  src/kg_core/src/face_detection_decode.cc
  src/kg_core/src/face_mesh_decode.cc
  src/kg_core/src/roi_align.cc
  src/kg_core/src/frame_convert.cc
  src/kg_core/src/png_write.cc)
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
    src/kg_tests/settings_test.cc
    src/kg_tests/presets_test.cc
    src/kg_tests/face_anchors_test.cc
    src/kg_tests/face_detection_decode_test.cc
    src/kg_tests/face_mesh_decode_test.cc
    src/kg_tests/roi_align_test.cc
    src/kg_tests/frame_convert_test.cc
    src/kg_tests/png_write_test.cc)
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

  # Windows: ONNX Runtime (MIT) — CPU provider for the 468-pt landmark
  # pipeline (M1b). Download pinned by SHA-256 at configure time; the zip has
  # no CMakeLists so it is fetched via file(DOWNLOAD) + file(ARCHIVE_EXTRACT).
  set(ORT_VERSION 1.31.1)
  set(ORT_ZIP_NAME "onnxruntime-win-x64-${ORT_VERSION}.zip")
  set(ORT_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/${ORT_ZIP_NAME}")
  set(ORT_EXPECTED_SHA256 "5869e26bb25314375f2d5ef1877e3ea06b1543d6c710e6e4a1ec2f7177ab4b2c")
  set(ORT_DEPS_DIR ${CMAKE_BINARY_DIR}/_deps)
  set(ORT_ROOT ${ORT_DEPS_DIR}/onnxruntime-win-x64-${ORT_VERSION})
  if(NOT EXISTS ${ORT_ROOT}/lib/onnxruntime.lib)
    set(ORT_ZIP ${ORT_DEPS_DIR}/${ORT_ZIP_NAME})
    if(NOT EXISTS ${ORT_ZIP})
      message(STATUS "Downloading ONNX Runtime ${ORT_VERSION} (82 MB, SHA-256 pinned)…")
      file(DOWNLOAD ${ORT_URL} ${ORT_ZIP}
           EXPECTED_HASH SHA256=${ORT_EXPECTED_SHA256}
           STATUS ORT_DL_STATUS SHOW_PROGRESS)
      list(GET ORT_DL_STATUS 0 ORT_DL_CODE)
      list(GET ORT_DL_STATUS 1 ORT_DL_MSG)
      if(NOT ORT_DL_CODE EQUAL 0)
        message(FATAL_ERROR "ONNX Runtime download failed (${ORT_DL_CODE}: ${ORT_DL_MSG}). "
                            "Delete ${ORT_ZIP} and retry, or set ORT_ROOT to a local unpacked copy.")
      endif()
    endif()
    message(STATUS "Extracting ${ORT_ZIP_NAME}…")
    file(ARCHIVE_EXTRACT INPUT ${ORT_ZIP} DESTINATION ${ORT_DEPS_DIR})
  endif()
  set(ORT_INCLUDE_DIR ${ORT_ROOT}/include)
  set(ORT_LIBRARY ${ORT_ROOT}/lib/onnxruntime.lib)
  set(ORT_DLL ${ORT_ROOT}/lib/onnxruntime.dll)
  if(NOT EXISTS ${ORT_DLL})
    set(ORT_DLL ${ORT_ROOT}/bin/onnxruntime.dll)
    set(ORT_LIBRARY ${ORT_ROOT}/lib/onnxruntime.lib)  # lib/ layout is standard for win-x64
  endif()
  if(NOT EXISTS ${ORT_INCLUDE_DIR}/onnxruntime_cxx_api.h OR NOT EXISTS ${ORT_LIBRARY} OR NOT EXISTS ${ORT_DLL})
    message(FATAL_ERROR "ONNX Runtime unpack incomplete under ${ORT_ROOT}")
  endif()
  add_library(onnxruntime SHARED IMPORTED GLOBAL)
  set_target_properties(onnxruntime PROPERTIES
    IMPORTED_LOCATION ${ORT_DLL}
    IMPORTED_IMPLIB ${ORT_LIBRARY}
    INTERFACE_INCLUDE_DIRECTORIES ${ORT_INCLUDE_DIR})

  # Pipeline (GPUPixel wrapper). The landmark provider is an interface;
  # OrtLandmarkProvider (Windows; 468-pt mesh) is the production path.
  if(KG_BUILD_GPUPIXEL_PIPELINE)
    add_library(kg_pipeline STATIC
      src/kg_pipeline/face_tracker.cc
      src/kg_pipeline/beauty_pipeline.cc
      src/kg_pipeline/ort_landmark_provider.cc)
    target_include_directories(kg_pipeline PUBLIC src/kg_pipeline)
    target_link_libraries(kg_pipeline PUBLIC
      kg_core gpupixel glad glfw onnxruntime winhttp bcrypt)
  endif()

  if(KG_BUILD_APP)
    add_executable(KameraGeulis WIN32
      src/kg_app/main.cc
      src/kg_app/panel.cc
      src/kg_app/startup.cc
      src/kg_app/selftest.cc)
    target_include_directories(KameraGeulis PRIVATE src/kg_app)
    target_link_libraries(KameraGeulis PRIVATE
      kg_core kg_capture kg_vcam kg_pipeline
      imgui_kg glad glfw opengl32 gdi32 dbghelp)
    # Ship onnxruntime.dll next to the exe (CPU landmark provider).
    add_custom_command(TARGET KameraGeulis POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ORT_DLL} $<TARGET_FILE_DIR:KameraGeulis>/onnxruntime.dll)
    if(EXISTS ${ORT_ROOT}/lib/onnxruntime_providers_shared.dll)
      add_custom_command(TARGET KameraGeulis POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ORT_ROOT}/lib/onnxruntime_providers_shared.dll $<TARGET_FILE_DIR:KameraGeulis>/onnxruntime_providers_shared.dll)
    endif()
  endif()
endif()
