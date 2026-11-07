@echo off
rem Equivalent of scripts/linux/build_ffmpeg_ps4.sh
rem Usage:  scripts/windows/build_ffmpeg_ps4.bat
setlocal EnableDelayedExpansion

for %%p in ("%~dp0..\..") do set "REPO_DIR=%%~fp"
call "%~dp0env.bat"
if errorlevel 1 exit /b 1

rem Git Bash locations (Git for Windows, or MSYS2).
set "GIT_BASH=C:\Program Files\Git\bin\bash.exe"
if exist "%GIT_BASH%" goto :bash_found
set "GIT_BASH=C:\Program Files\Git\usr\bin\bash.exe"
if exist "%GIT_BASH%" goto :bash_found
echo error: Git Bash not found. Install Git for Windows or MSYS2, then re-run. 1>&2
exit /b 1
:bash_found

set "OO_MSYS=%OO_PS4_TOOLCHAIN:\=/%"
set "REPO_MSYS=%REPO_DIR:\=/%"
set "USER_MSYS=%USERPROFILE:\=/%"
set "OO_PS4_TOOLCHAIN=%OO_MSYS%"

echo build_ffmpeg_ps4: using %GIT_BASH%
echo OO_PS4_TOOLCHAIN=%OO_PS4_TOOLCHAIN% ^(%OO_MSYS%^)

"%GIT_BASH%" -lc "HOME='%USER_MSYS%' bash '%REPO_MSYS%/scripts/linux/build_ffmpeg_ps4.sh'"
if errorlevel 1 exit /b 1

echo FFmpeg installed at %USERPROFILE%\ps4dev\ffmpeg-ps4
exit /b 0
