cmake_minimum_required(VERSION 3.21)

# vcpkg manifest mode: the triplet must be known before project() so the
# toolchain can resolve dependencies. Presets supply it; default to static x64
# when configured without a preset.
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
  set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "vcpkg triplet")
endif()

project(ArchiveExtractor
  VERSION 0.1.0
  DESCRIPTION "macOS-Archive-Utility-style archive extractor for Windows"
  LANGUAGES CXX)

# --- Global settings -------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# x64 only (design decision; matches the static x64 triplet).
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "ArchiveExtractor is x64-only; configure with a 64-bit toolset.")
endif()

# Static CRT (/MT[d]) to match the x64-windows-static triplet and avoid a VC++
# runtime redistributable dependency at install time.
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc)
endif()
add_compile_definitions(UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

enable_testing()

# ---------------------------------------------------------------------------
# archive_core — GUI-free static library holding detection / engine / layout /
# CLI logic so it is unit-testable without the Win32 front end. The exe and the
# test target both link it.
# ---------------------------------------------------------------------------
add_library(archive_core STATIC
  src/core/cli.cpp
  src/core/paths.cpp
  src/core/logging.cpp
  src/core/detect.cpp
)
target_include_directories(archive_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(archive_core PUBLIC cxx_std_20)

# ---------------------------------------------------------------------------
# ArchiveExtractor.exe — Win32 GUI-subsystem front end.
# The .manifest source is embedded by the linker (DPI v2 + comctl6 + longPath).
# ---------------------------------------------------------------------------
add_executable(ArchiveExtractor WIN32
  src/app/main.cpp
  src/app/app.manifest
)
target_link_libraries(ArchiveExtractor PRIVATE archive_core ole32 user32 shell32)
# wWinMain (wide entry) requires the wide CRT startup under /SUBSYSTEM:WINDOWS.
target_link_options(ArchiveExtractor PRIVATE /ENTRY:wWinMainCRTStartup)

# ---------------------------------------------------------------------------
# Unit tests (GoogleTest via vcpkg, wired into CTest).
# ---------------------------------------------------------------------------
find_package(GTest CONFIG REQUIRED)
add_executable(archive_core_tests
  tests/test_cli.cpp
  tests/test_paths.cpp
  tests/test_detect.cpp
)
target_link_libraries(archive_core_tests PRIVATE archive_core GTest::gtest GTest::gtest_main)
# Real header fixtures live in the generated corpus under tests/assets. Tests
# read leading bytes of these via an absolute path baked in at configure time.
target_compile_definitions(archive_core_tests PRIVATE
  AE_TEST_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/assets")

include(GoogleTest)
gtest_discover_tests(archive_core_tests)
