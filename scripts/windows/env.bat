@echo off
rem Equivalent of scripts/linux/env.sh

if defined OO_PS4_TOOLCHAIN goto :toolchain_done
if defined OO_PS4_TOOLCHAIN_SRC set "OO_PS4_TOOLCHAIN=%OO_PS4_TOOLCHAIN_SRC%"
:toolchain_done

if not defined OO_PS4_TOOLCHAIN (
    echo error: set OO_PS4_TOOLCHAIN ^(or OO_PS4_TOOLCHAIN_SRC^) to your OpenOrbis SDK path 1>&2
    exit /b 1
)

set "DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1"
set "DOTNET_ROLL_FORWARD=LatestMajor"

if not exist "C:\Program Files\LLVM\bin\clang.exe" goto :llvm_done
echo "%PATH%" | findstr /i /c:"C:\Program Files\LLVM\bin" >nul
if not errorlevel 1 goto :llvm_done
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
:llvm_done

echo OO_PS4_TOOLCHAIN=%OO_PS4_TOOLCHAIN%
exit /b 0
