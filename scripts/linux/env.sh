#!/usr/bin/env bash
# Development environment for moonlight-ps4. Load with: source scripts/linux/env.sh

export OO_PS4_TOOLCHAIN="${OO_PS4_TOOLCHAIN:-$HOME/ps4dev/OpenOrbis/PS4Toolchain}"

# PkgTool.Core is old .NET: no ICU, and needs a local OpenSSL 1.1 extract.
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
export PS4_HOSTLIBS="${PS4_HOSTLIBS:-$HOME/ps4dev/hostlibs/usr/lib}"

echo "OO_PS4_TOOLCHAIN=$OO_PS4_TOOLCHAIN"
