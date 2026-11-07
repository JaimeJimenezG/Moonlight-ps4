@echo off
rem Configure, build, and package moonlight-ps4 for PS4 on Windows.
rem Equivalent of scripts\linux\build_pkg.sh.
rem Usage:
rem   scripts\windows\build_pkg.bat            build PS4 .pkg
rem   scripts\windows\build_pkg.bat --clean    reconfigure from scratch
rem   set BUILD_DIR=build-ps4-debug ^& scripts\windows\build_pkg.bat
rem Requirements: call scripts\windows\env.bat (done automatically), PS4 FFmpeg
rem at %%USERPROFILE%%\ps4dev\ffmpeg-ps4 (see scripts\windows\build_ffmpeg_ps4.bat).
setlocal EnableDelayedExpansion

for %%p in ("%~dp0..\..") do set "REPO_DIR=%%~fp"
call "%~dp0env.bat"
if errorlevel 1 exit /b 1

if not defined BUILD_DIR set "BUILD_DIR=%REPO_DIR%\build-ps4"
for %%p in ("%BUILD_DIR%") do set "BUILD_DIR=%%~fp"
set "TOOLCHAIN=%REPO_DIR%\cmake\openorbis.cmake"
if not defined FFMPEG_PS4_PREFIX set "FFMPEG_PS4_PREFIX=%USERPROFILE%\ps4dev\ffmpeg-ps4"
for %%p in ("%FFMPEG_PS4_PREFIX%") do set "FFMPEG_PS4_PREFIX=%%~fp"

rem Version is defined in scripts\windows\make_pkg.bat (keep in sync with scripts\linux\make_pkg.sh).
set "VERSION=1.1.0"

set "CLEAN=0"
:parse_args
if [%1]==[] goto :parse_done
if [%1]==[-h] goto :help
if [%1]==[--help] goto :help
if [%1]==[--clean] set "CLEAN=1"
if [%1]==[--hw] set "ENABLE_HW=1"
if [%1]==[--no-hw] set "ENABLE_HW=0"
if [%1]==[--clean] shift & goto :parse_args
if [%1]==[--hw] shift & goto :parse_args
if [%1]==[--no-hw] shift & goto :parse_args
echo Unknown option: %~1 1>&2
goto :help
:parse_done

if not exist "%TOOLCHAIN%" (
    echo error: toolchain not found: %TOOLCHAIN% 1>&2
    exit /b 1
)

if not exist "%FFMPEG_PS4_PREFIX%\lib\libavcodec.a" (
    echo error: PS4 FFmpeg not found at %FFMPEG_PS4_PREFIX% 1>&2
    echo Run: %REPO_DIR%\scripts\windows\build_ffmpeg_ps4.bat 1>&2
    exit /b 1
)

rem Prefer the Visual Studio-bundled CMake (understands Windows paths);
rem the MSYS2 cmake on PATH mangles drive-letter paths. Same for Ninja.
rem NOTE: expand %PATH% only outside parenthesized blocks (it may contain
rem parenthesized segments such as "Program Files (x86)").
set "VS_CMAKE_BIN=C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "VS_NINJA_BIN=C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
if exist "%VS_CMAKE_BIN%\cmake.exe" set "PATH=%VS_CMAKE_BIN%;%PATH%"
if exist "%VS_NINJA_BIN%\ninja.exe" set "PATH=%VS_NINJA_BIN%;%PATH%"
where ninja >nul 2>nul
if %errorlevel% equ 0 (
    set "GENERATOR=Ninja"
) else (
    set "GENERATOR=Visual Studio 17 2022"
)

echo build: ML_ENABLE_VIDEODEC2=1 (prefer_hw in moonlight.ini)

if %CLEAN% equ 1 (
    echo build: cleaning %BUILD_DIR%
    if exist "%BUILD_DIR%\" rmdir /s /q "%BUILD_DIR%"
)

echo build: configure -^> %BUILD_DIR%
cmake -S "%REPO_DIR%" -B "%BUILD_DIR%" -G "%GENERATOR%" -DCMAKE_TOOLCHAIN_FILE="%TOOLCHAIN%" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFFMPEG_PS4_PREFIX="%FFMPEG_PS4_PREFIX%"
if errorlevel 1 exit /b 1

echo build: compile + pkg
cmake --build "%BUILD_DIR%"
if errorlevel 1 exit /b 1

set "PKG=%BUILD_DIR%\Moonlight-%VERSION%.pkg"
if not exist "%PKG%" (
    echo error: did not produce %PKG% 1>&2
    exit /b 1
)

echo OK: %PKG%
exit /b 0

:help
echo Usage:
echo   scripts\windows\build_pkg.bat
echo   scripts\windows\build_pkg.bat --clean
echo   set BUILD_DIR=build-ps4-debug ^& scripts\windows\build_pkg.bat
exit /b 0
