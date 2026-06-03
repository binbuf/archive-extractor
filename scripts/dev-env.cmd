@echo off
REM Sets up the VS x64 toolchain + VCPKG_ROOT for one-shot CMake invocations.
REM Usage:  scripts\dev-env.cmd <command...>
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if not defined VCPKG_ROOT set "VCPKG_ROOT=C:\vcpkg"
%*
