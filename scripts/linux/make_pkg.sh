#!/usr/bin/env bash
# Build the installable .pkg from an already-created eboot.bin.
# Usage: make_pkg.sh <eboot.bin> <output_dir>
set -euo pipefail

EBOOT="$1"
OUTDIR="$2"

REPO_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
TOOLS="$OO_PS4_TOOLCHAIN/bin/linux"

TITLE="Moonlight PS4"
VERSION="1.1.0"
TITLE_ID="MLNT00001"
CONTENT_ID="IV0000-MLNT00001_00-MOONLIGHTPS40000"

export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
export LD_LIBRARY_PATH="${PS4_HOSTLIBS:-$HOME/ps4dev/hostlibs/usr/lib}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

STAGE="$OUTDIR/pkg-stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/sce_sys/about" "$STAGE/sce_module" "$STAGE/assets/misc"

# Allowlist only — never copy runtime dumps / certs / logs from pkg/assets/misc.
cp "$REPO_DIR/pkg/sce_sys/icon0.png" "$STAGE/sce_sys/"
cp "$REPO_DIR/pkg/sce_sys/about/right.sprx" "$STAGE/sce_sys/about/"
cp "$REPO_DIR/pkg/sce_module/libc.prx" "$STAGE/sce_module/"
cp "$REPO_DIR/pkg/sce_module/libSceFios2.prx" "$STAGE/sce_module/"
cp "$REPO_DIR/pkg/assets/misc/moonlight.ini" "$STAGE/assets/misc/"
cp "$EBOOT" "$STAGE/eboot.bin"

SFO="$STAGE/sce_sys/param.sfo"
"$TOOLS/PkgTool.Core" sfo_new "$SFO"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" APP_TYPE  --type Integer --maxsize 4   --value 1
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" APP_VER   --type Utf8    --maxsize 8   --value "$VERSION"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" ATTRIBUTE --type Integer --maxsize 4   --value 0
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" CATEGORY  --type Utf8    --maxsize 4   --value 'gd'
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" CONTENT_ID --type Utf8   --maxsize 48  --value "$CONTENT_ID"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" SYSTEM_VER --type Integer --maxsize 4  --value 0
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" TITLE     --type Utf8    --maxsize 128 --value "$TITLE"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" TITLE_ID  --type Utf8    --maxsize 12  --value "$TITLE_ID"
"$TOOLS/PkgTool.Core" sfo_setentry "$SFO" VERSION   --type Utf8    --maxsize 8   --value "$VERSION"

cd "$STAGE"
FILES="eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png"
FILES="$FILES sce_module/libc.prx sce_module/libSceFios2.prx"
FILES="$FILES assets/misc/moonlight.ini"

"$TOOLS/create-gp4" -out pkg.gp4 --content-id="$CONTENT_ID" --files "$FILES"
"$TOOLS/PkgTool.Core" pkg_build pkg.gp4 "$OUTDIR"

PKG_ID="$OUTDIR/$CONTENT_ID.pkg"
PKG_NAME="$OUTDIR/Moonlight-${VERSION}.pkg"
cp -f "$PKG_ID" "$PKG_NAME"
echo "OK: $PKG_NAME"
