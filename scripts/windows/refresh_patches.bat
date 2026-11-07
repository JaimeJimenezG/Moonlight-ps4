@echo off
rem Equivalent of scripts/linux/refresh_patches.sh.
setlocal EnableDelayedExpansion

for %%p in ("%~dp0..\..") do set "REPO_DIR=%%~fp"
set "DEPS_FILE=%REPO_DIR%\third_party\DEPS"
set "PATCH_DIR=%REPO_DIR%\patches"

call :pin_for "third_party/moonlight-common-c" MCC_PIN
if errorlevel 1 exit /b 1
call :pin_for "third_party/moonlight-common-c/enet" ENET_PIN
if errorlevel 1 exit /b 1

call :diff_against_pin "%REPO_DIR%\third_party\moonlight-common-c" "%MCC_PIN%" "%PATCH_DIR%\moonlight-common-c-orbis.patch" "src/"
if errorlevel 1 exit /b 1

call :diff_against_pin "%REPO_DIR%\third_party\moonlight-common-c\enet" "%ENET_PIN%" "%PATCH_DIR%\moonlight-common-c-enet-orbis.patch" "unix.c"
if errorlevel 1 exit /b 1

echo refresh_patches: done - review patches\ then commit those files only
exit /b 0

:pin_for
set "WANT=%~1"
set "%~2="
for /F "tokens=1,2,3,4" %%a in ('type "%DEPS_FILE%" ^| findstr /R /V "^[ ]*#" ^| findstr /R /V "^[ ]*$"') do (
    if "%%b"=="%WANT%" set "%~2=%%d"
)
if "%~2"=="" exit /b 1
exit /b 0

:diff_against_pin
set "TREE=%~1"
set "PIN=%~2"
set "OUT=%~3"
set "PATHSPEC=%~4"
for /F "tokens=*" %%s in ('git -C "%TREE%" rev-parse %PIN%^{commit}') do set "SHA=%%s"
git -C "%TREE%" -c diff.noprefix=false diff --ignore-submodules=all --src-prefix=a/ --dst-prefix=b/ "%SHA%" -- "%PATHSPEC%" > "%OUT%"
if errorlevel 1 exit /b 1
for %%f in ("%OUT%") do set "OUT_SIZE=%%~z"
echo refresh_patches: wrote %OUT% (%OUT_SIZE% bytes)
exit /b 0
