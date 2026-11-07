@echo off
rem Equivalent of scripts/linux/setup_deps.sh.
rem Usage:
rem   scripts/windows/setup_deps.bat | init/update to pins + apply patches
rem   scripts/windows/setup_deps.bat --no-patch
setlocal EnableDelayedExpansion

for %%p in ("%~dp0..\..") do set "REPO_DIR=%%~fp"
set "DEPS_FILE=%REPO_DIR%\third_party\DEPS"
set "APPLY_PATCHES=1"

if [%1]==[-h] goto :usage
if [%1]==[--help] goto :usage
if [%1]==[--no-patch] set "APPLY_PATCHES=0"
if not [%1]==[] if not [%1]==[--no-patch] if not [%1]==[-h] if not [%1]==[--help] (
    echo setup_deps: unknown option: %~1 1>&2
    exit /b 1
)

if not exist "%DEPS_FILE%" (
    echo setup_deps: missing %DEPS_FILE% 1>&2
    exit /b 1
)

echo setup_deps: reading %DEPS_FILE%

rem First pass: top-level deps (not nested under moonlight-common-c/*).
for /F "tokens=1,2,3,4" %%a in ('type "%DEPS_FILE%" ^| findstr /R /V "^[ ]*#" ^| findstr /R /V "^[ ]*$"') do (
    if not "%%b"=="" (
        echo %%b| findstr /R /C:"^third_party/moonlight-common-c/" >nul
        if errorlevel 1 call :checkout_pin "%%a" "%%b" "%%c" "%%d"
    )
)

rem Nested pins (enet / nanors) after parent exists.
for /F "tokens=1,2,3,4" %%a in ('type "%DEPS_FILE%" ^| findstr /R /V "^[ ]*#" ^| findstr /R /V "^[ ]*$"') do (
    if not "%%b"=="" (
        echo %%b| findstr /R /C:"^third_party/moonlight-common-c/" >nul
        if not errorlevel 1 call :checkout_pin "%%a" "%%b" "%%c" "%%d"
    )
)

rem Sync parent-index gitlinks when this tree is a git checkout (optional).
git -C "%REPO_DIR%" rev-parse --is-inside-work-tree >nul 2>&1
if %errorlevel% equ 0 (
    git -C "%REPO_DIR%" submodule sync --recursive >nul 2>&1
    git -C "%REPO_DIR%" config submodule.third_party/moonlight-common-c.ignore dirty >nul 2>&1
)

if %APPLY_PATCHES% neq 1 (
    echo setup_deps: skipped patches (--no-patch)
    goto :deps_done
)
call "%~dp0apply_patches.bat"
if errorlevel 1 exit /b 1
:deps_done

echo setup_deps: done
exit /b 0

:usage
echo Usage:
echo   scripts\windows\setup_deps.bat
echo   scripts\windows\setup_deps.bat --no-patch
exit /b 0

:checkout_pin
set "NAME=%~1"
set "REL=%~2"
set "URL=%~3"
set "PIN=%~4"
set "ABS=%REPO_DIR%\%REL%"
if not exist "%ABS%\.git" (
    echo setup_deps: cloning %NAME% -^> %REL%
    for %%p in ("%ABS%") do set "PARENT=%%~dp"
    if not exist "%PARENT%" mkdir "%PARENT%"
    git clone --no-checkout "%URL%" "%ABS%"
    if errorlevel 1 exit /b 1
)
git -C "%ABS%" remote set-url origin "%URL%" >nul 2>&1
if errorlevel 1 git -C "%ABS%" remote add origin "%URL%" >nul 2>&1
if errorlevel 1 exit /b 1

call :resolve_pin "%ABS%" "%PIN%" SHA
if errorlevel 1 exit /b 1
echo setup_deps: %NAME% @ %SHA:~0,12% (pin %PIN%)
git -C "%ABS%" reset --hard "%SHA%" >nul
if errorlevel 1 exit /b 1
git -C "%ABS%" clean -fd >nul
if errorlevel 1 exit /b 1
exit /b 0

:resolve_pin
set "TREE=%~1"
set "PIN=%~2"
set "%~3="
git -C "%TREE%" rev-parse --verify "%PIN%^{commit}" >nul 2>&1
if %errorlevel% equ 0 (
    for /F "tokens=*" %%s in ('git -C "%TREE%" rev-parse "%PIN%^{commit}"') do set "%~3=%%s"
    exit /b 0
)
git -C "%TREE%" fetch --tags --force origin "+refs/heads/*:refs/remotes/origin/*" >nul 2>&1
git -C "%TREE%" fetch origin "%PIN%" >nul 2>&1
for /F "tokens=*" %%s in ('git -C "%TREE%" rev-parse --verify "%PIN%^{commit}" 2^>nul') do set "%~3=%%s"
if "%~3"=="" exit /b 1
exit /b 0
