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
  src/core/extract.cpp
  src/core/sevenzip.cpp
  src/core/layout.cpp
  src/core/retry.cpp
  src/core/error_model.cpp
  src/core/registration.cpp
)
target_include_directories(archive_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(archive_core PUBLIC cxx_std_20)

# libarchive (primary extraction backend) + ole32 for CoCreateGuid. PRIVATE:
# the public archive_core headers do not expose libarchive types, so consumers
# (exe, tests) need not see it — they just link the resulting static lib.
# shell32: SHGetKnownFolderPath (rotating-log path resolution in logging.cpp) +
#   SHChangeNotify / SHOpenWithDialog (registration.cpp).
# advapi32: registry (RegCreateKeyEx/RegSetValueEx/RegDeleteTree) + token
#   elevation check (OpenProcessToken/GetTokenInformation) in registration.cpp.
find_package(LibArchive REQUIRED)
# bit7z (task 09): static wrapper over the runtime 7z.dll. Provides the
# SevenZipExtractor backend for encrypted-7z and all RAR/RAR5 + their
# multi-volume chains. The vcpkg port exports unofficial::bit7z::bit7z64; it
# transitively pulls in 7zip::7zip (the static 7-zip SDK glue) and oleaut32.
find_package(unofficial-bit7z CONFIG REQUIRED)
target_link_libraries(archive_core PRIVATE LibArchive::LibArchive
  unofficial::bit7z::bit7z64 ole32 shell32 advapi32)

# ---------------------------------------------------------------------------
# Runtime 7z.dll. bit7z is a static wrapper that LoadLibrary's 7z.dll on demand
# (the first time SevenZipExtractor runs), so the DLL must sit next to whatever
# exe loads it. The vcpkg 7zip port is static-only (no DLL shipped), so we
# source 7z.dll from the local 7-Zip install (matched to the bit7z-built 7zip
# 24.09 SDK; 24.0x DLLs are ABI-compatible with bit7z 4.x). In task 13 this same
# DLL goes into the MSI payload.
#
# AE_7ZDLL is resolved at configure time; copied next to both the app exe and
# the test exe via a POST_BUILD step (see targets below).
set(_ae_7zdll_candidates
  "$ENV{ProgramFiles}/7-Zip/7z.dll"
  "$ENV{ProgramW6432}/7-Zip/7z.dll"
  "C:/Program Files/7-Zip/7z.dll")
set(AE_7ZDLL "")
foreach(_cand IN LISTS _ae_7zdll_candidates)
  if(EXISTS "${_cand}")
    set(AE_7ZDLL "${_cand}")
    break()
  endif()
endforeach()
if(AE_7ZDLL STREQUAL "")
  message(WARNING
    "7z.dll not found; the 7z.dll-backed (bit7z) extraction paths and tests "
    "will fail at runtime. Install 7-Zip or set AE_7ZDLL.")
else()
  message(STATUS "Using runtime 7z.dll: ${AE_7ZDLL}")
endif()

# ---------------------------------------------------------------------------
# ArchiveExtractor.exe — Win32 GUI-subsystem front end.
# The .manifest source is embedded by the linker (DPI v2 + comctl6 + longPath).
# ---------------------------------------------------------------------------
add_executable(ArchiveExtractor WIN32
  src/app/main.cpp
  src/app/progress_dialog.cpp
  src/app/error_dialog.cpp
  src/app/shell_reveal.cpp
  src/app/app.manifest
)
# comctl32: themed progress bar / button (v6 via the manifest).
# shlwapi: PathFindFileNameW for the archive label.
# oleaut32: VariantInit/VariantClear used by the IShellWindows reveal path.
target_link_libraries(ArchiveExtractor PRIVATE
  archive_core ole32 oleaut32 user32 shell32 comctl32 shlwapi)
# wWinMain (wide entry) requires the wide CRT startup under /SUBSYSTEM:WINDOWS.
target_link_options(ArchiveExtractor PRIVATE /ENTRY:wWinMainCRTStartup)

# Ship 7z.dll next to the app exe (bit7z loads it on demand at runtime).
if(NOT AE_7ZDLL STREQUAL "")
  add_custom_command(TARGET ArchiveExtractor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${AE_7ZDLL}" "$<TARGET_FILE_DIR:ArchiveExtractor>/7z.dll"
    COMMENT "Copying 7z.dll next to ArchiveExtractor.exe")
endif()

# ---------------------------------------------------------------------------
# Unit tests (GoogleTest via vcpkg, wired into CTest).
# ---------------------------------------------------------------------------
find_package(GTest CONFIG REQUIRED)
add_executable(archive_core_tests
  tests/test_cli.cpp
  tests/test_paths.cpp
  tests/test_detect.cpp
  tests/test_extract.cpp
  tests/test_sevenzip.cpp
  tests/test_layout.cpp
  tests/test_error_model.cpp
  tests/test_registration.cpp
)
target_link_libraries(archive_core_tests PRIVATE archive_core GTest::gtest GTest::gtest_main)
# Real header fixtures live in the generated corpus under tests/assets. Tests
# read leading bytes of these via an absolute path baked in at configure time.
target_compile_definitions(archive_core_tests PRIVATE
  AE_TEST_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/assets")

# The 7z.dll-backed tests need 7z.dll next to the test exe too.
if(NOT AE_7ZDLL STREQUAL "")
  add_custom_command(TARGET archive_core_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${AE_7ZDLL}" "$<TARGET_FILE_DIR:archive_core_tests>/7z.dll"
    COMMENT "Copying 7z.dll next to archive_core_tests.exe")
endif()

include(GoogleTest)
gtest_discover_tests(archive_core_tests)
