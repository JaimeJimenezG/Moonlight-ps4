@echo off
rem Equivalent of scripts\linux\apply_patches.sh.
rem Usage:  scripts\windows\apply_patches.bat
setlocal EnableDelayedExpansion

for %%p in ("%~dp0..\..") do set "REPO_DIR=%%~fp"
set "PATCH_DIR=%REPO_DIR%\patches"

call :apply_one "%REPO_DIR%\third_party\moonlight-common-c" "%PATCH_DIR%\moonlight-common-c-orbis.patch" "moonlight-common-c Orbis"
if errorlevel 1 exit /b 1
call :apply_one "%REPO_DIR%\third_party\moonlight-common-c\enet" "%PATCH_DIR%\moonlight-common-c-enet-orbis.patch" "enet Orbis"
if errorlevel 1 exit /b 1

echo apply_patches: done
exit /b 0

:apply_one
set "TREE=%~1"
set "PATCH=%~2"
set "LABEL=%~3"
if not exist "%TREE%\" (
    echo apply_patches: missing %TREE% ^(run scripts\windows\setup_deps.bat first^) 1>&2
    exit /b 1
)
if not exist "%PATCH%" (
    echo apply_patches: missing patch %PATCH% 1>&2
    exit /b 1
)
git -C "%TREE%" apply --check "%PATCH%" >nul 2>&1
if %errorlevel% equ 0 (
    git -C "%TREE%" apply "%PATCH%"
    if errorlevel 1 exit /b 1
    echo apply_patches: applied %LABEL%
    exit /b 0
)
git -C "%TREE%" apply --reverse --check "%PATCH%" >nul 2>&1
if %errorlevel% equ 0 (
    echo apply_patches: already applied %LABEL%
    exit /b 0
)
echo apply_patches: FAILED %LABEL% (tree dirty or pin mismatch?) 1>&2
echo   tree=%TREE% 1>&2
echo   patch=%PATCH% 1>&2
exit /b 1
