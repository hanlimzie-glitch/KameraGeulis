# Pinned third-party deps (FetchContent). Nothing is vendored in-tree.
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# nlohmann/json (MIT) — settings persistence
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(nlohmann_json)

# Catch2 (BSL-1.0) — tests only
if(KG_BUILD_TESTS)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(Catch2)
endif()

if(WIN32)
  # GPUPixel (Apache-2.0) — engine. Windows: face detector OFF (no public
  # Windows Mars-Face binary; upstream builds Windows the same way).
  set(GPUPIXEL_ENABLE_FACE_DETECTOR OFF CACHE BOOL "" FORCE)
  set(GPUPIXEL_BUILD_DESKTOP_DEMO OFF CACHE BOOL "" FORCE)
  set(GPUPIXEL_INSTALL OFF CACHE BOOL "" FORCE)
  set(GPUPIXEL_BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
  FetchContent_Declare(gpupixel
    GIT_REPOSITORY https://github.com/pixpark/gpupixel.git
    GIT_TAG        f33abfa5a802e0dfb04b98f9240a4f24b9d0eb96)  # v1.3.1
  FetchContent_MakeAvailable(gpupixel)
  # Debug symbols for crash symbolization (map file lets CI resolve RVAs like
  # the gpupixel.dll 0x3968a crash site against functions). Keeps /MD Release.
  # /DEBUG can change the linker's /OPT defaults (REF/ICF), which would MOVE
  # every function and make the map useless for symbolizing the crash RVA from
  # the user's pre-M2d dll - so /OPT:REF /OPT:ICF are re-stated explicitly to
  # keep .text layout identical to the build that crashed at 0x3968a.
  if(MSVC)
    target_compile_options(gpupixel PRIVATE /Zi)
    target_link_options(gpupixel PRIVATE /DEBUG /MAP /OPT:REF /OPT:ICF)
  endif()

  # Dear ImGui (MIT) — pinned v1.91.9, same version GPUPixel vendors
  FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.9
    GIT_SHALLOW    TRUE)
  FetchContent_GetProperties(imgui)
  if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
    add_library(imgui_kg STATIC
      ${imgui_SOURCE_DIR}/imgui.cpp
      ${imgui_SOURCE_DIR}/imgui_draw.cpp
      ${imgui_SOURCE_DIR}/imgui_tables.cpp
      ${imgui_SOURCE_DIR}/imgui_widgets.cpp
      ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
      ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
    target_include_directories(imgui_kg PUBLIC
      ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
    target_link_libraries(imgui_kg PUBLIC glfw)
  endif()
endif()
