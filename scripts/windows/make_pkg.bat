@echo off
rem Equivalent of scripts/linux/make_pkg.sh
rem Usage: make_pkg.bat <eboot.bin> <output_dir>
setlocal EnableDelayedExpansion

set "EBOOT=%~1"
set "OUTDIR=%~2"
if "%EBOOT%"=="" goto :usage
if "%OUTDIR%"=="" goto :usage

for %%p in ("%~dp0..\..") do set "REPO_DIR=%%~fp"
if not defined OO_PS4_TOOLCHAIN call "%~dp0env.bat"
if not defined OO_PS4_TOOLCHAIN (
    echo error: OO_PS4_TOOLCHAIN or OO_PS4_TOOLCHAIN_SRC must point at the SDK 1>&2
    exit /b 1
)

set "TOOLS=%OO_PS4_TOOLCHAIN%\bin\windows"
set "TITLE=Moonlight PS4"
set "VERSION=1.1.0"
set "TITLE_ID=MLNT00001"
set "CONTENT_ID=IV0000-MLNT00001_00-MOONLIGHTPS40000"

set "DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1"
set "DOTNET_ROLL_FORWARD=LatestMajor"

set "STAGE=%OUTDIR%\pkg-stage"
if exist "%STAGE%\" rmdir /s /q "%STAGE%"
mkdir "%STAGE%\sce_sys\about" >nul
mkdir "%STAGE%\sce_module" >nul
mkdir "%STAGE%\assets\misc" >nul

call :copy1 "%REPO_DIR%\pkg\sce_sys\icon0.png" "%STAGE%\sce_sys\icon0.png"
if errorlevel 1 exit /b 1
call :copy1 "%REPO_DIR%\pkg\sce_sys\about\right.sprx" "%STAGE%\sce_sys\about\right.sprx"
if errorlevel 1 exit /b 1
call :copy1 "%REPO_DIR%\pkg\sce_module\libc.prx" "%STAGE%\sce_module\libc.prx"
if errorlevel 1 exit /b 1
call :copy1 "%REPO_DIR%\pkg\sce_module\libSceFios2.prx" "%STAGE%\sce_module\libSceFios2.prx"
if errorlevel 1 exit /b 1
call :copy1 "%REPO_DIR%\pkg\assets\misc\moonlight.ini" "%STAGE%\assets\misc\moonlight.ini"
if errorlevel 1 exit /b 1
call :copy1 "%EBOOT%" "%STAGE%\eboot.bin"
if errorlevel 1 exit /b 1

set "SFO=%STAGE%\sce_sys\param.sfo"
set "PKGTOOL=%TOOLS%\PkgTool.Core.exe"
set "CREATE_GP4=%TOOLS%\create-gp4.exe"

"%PKGTOOL%" sfo_new "%SFO%"
if errorlevel 1 exit /b 1
"%PKGTOOL%" sfo_setentry "%SFO%" APP_TYPE --type Integer --maxsize 4 --value 1
if errorlevel 1 exit /b 1
"%PKGTOOL%" sfo_setentry "%SFO%" APP_VER --type Utf8 --maxsize 8 --value "%VERSION%"
if errorlevel 1 exit /b 1
"%PKGTOOL%" sfo_setentry "%SFO%" ATTRIBUTE --type Integer --maxsize 4 --value 0
if errorlevel 1 exit /b 1
"%PKGTOOL%" sfo_setentry "%SFO%" CATEGORY --type Utf8 --maxsize 4 --value gd
if errorlevel 1 exit /b 1
"%PKGTOOL%" sfo_setentry "%SFO%" CONTENT_ID --type Utf8 --maxsize 48 --value "%CONTENT_ID%"
if errorlevel 1 exit /b 1
"%PKGTOOL%" sfo_setentry "%SFO%" DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
if errorlevel 1 exit /b 1
"%PKGTOOL%" sfo_setentry "%SFO%" SYSTEM_VER --type Integer --maxsize 4 --value 0
if errorlevel 1 exit /b 1
"%PKGTOOL%" sfo_setentry "%SFO%" TITLE --type Utf8 --maxsize 128 --value "%TITLE%"
if errorlevel 1 exit /b 1
"%PKGTOOL%" sfo_setentry "%SFO%" TITLE_ID --type Utf8 --maxsize 12 --value "%TITLE_ID%"
if errorlevel 1 exit /b 1
"%PKGTOOL%" sfo_setentry "%SFO%" VERSION --type Utf8 --maxsize 8 --value "%VERSION%"
if errorlevel 1 exit /b 1

pushd "%STAGE%"
if errorlevel 1 exit /b 1
set "FILES=eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_module/libc.prx sce_module/libSceFios2.prx assets/misc/moonlight.ini"
"%CREATE_GP4%" -out pkg.gp4 --content-id="%CONTENT_ID%" --files "%FILES%"
if errorlevel 1 (
    popd
    exit /b 1
)
"%PKGTOOL%" pkg_build pkg.gp4 "%OUTDIR%"
if errorlevel 1 (
    popd
    exit /b 1
)
popd

set "PKG_ID=%OUTDIR%\%CONTENT_ID%.pkg"
set "PKG_NAME=%OUTDIR%\Moonlight-%VERSION%.pkg"
if not exist "%PKG_ID%" (
    echo error: did not produce %PKG_ID% 1>&2
    exit /b 1
)
copy /y "%PKG_ID%" "%PKG_NAME%" >nul
if errorlevel 1 exit /b 1
echo OK: %PKG_NAME%
exit /b 0

:usage
echo Usage: scripts\windows\make_pkg.bat ^<eboot.bin^> ^<output_dir^> 1>&2
exit /b 1

:copy1
copy /y "%~1" "%~2" >nul
if errorlevel 1 (
    echo error: copy %~1 -> %~2 failed 1>&2
    exit /b 1
)
exit /b 0
