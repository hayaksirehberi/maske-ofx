#!/usr/bin/env bash
#
# Builds a double-clickable macOS installer (.pkg) for the Maske OFX plugin.
# The installer drops HistoryBrush.ofx.bundle into /Library/OFX/Plugins and
# asks the user for their admin password through the standard Installer UI —
# no Terminal, no folder wrangling.
#
# Usage:
#   installer/macos/build_pkg.sh [path/to/HistoryBrush.ofx.bundle]
#
# If no bundle path is given, it looks for build/HistoryBrush.ofx.bundle.
#
# Optional signing (recommended for distribution to other machines):
#   INSTALLER_SIGN_ID="Developer ID Installer: Your Name (TEAMID)" \
#       installer/macos/build_pkg.sh
#
set -euo pipefail

VERSION="1.2"
IDENTIFIER="com.mustafaekinci.Maske"
INSTALL_LOCATION="/Library/OFX/Plugins"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$HERE/../.." && pwd)"

BUNDLE="${1:-$REPO_ROOT/build/HistoryBrush.ofx.bundle}"
if [[ ! -d "$BUNDLE" ]]; then
    echo "error: bundle not found: $BUNDLE" >&2
    echo "       build it first, or pass the path as the first argument." >&2
    exit 1
fi

OUT_DIR="$REPO_ROOT/dist"
STAGING="$(mktemp -d)"
SCRATCH="$(mktemp -d)"
trap 'rm -rf "$STAGING" "$SCRATCH"' EXIT

mkdir -p "$OUT_DIR"

# Stage the payload at the layout it should have under the install location.
# ditto --noextattr --noqtn strips extended attributes so no AppleDouble (._*)
# entries end up in the payload. (com.apple.provenance, a protected system xattr
# macOS re-adds to executables, is harmless if it survives — it never lands as a
# visible ._ file on the installed APFS volume.)
ditto --noextattr --noqtn "$BUNDLE" "$STAGING/$(basename "$BUNDLE")"
find "$STAGING" -name '._*' -delete 2>/dev/null || true

# Verify the payload is a real universal OFX plugin before packaging.
OFX_BIN="$STAGING/HistoryBrush.ofx.bundle/Contents/MacOS/HistoryBrush.ofx"
if [[ ! -f "$OFX_BIN" ]]; then
    echo "error: bundle has no Contents/MacOS/HistoryBrush.ofx" >&2
    exit 1
fi
echo "==> Payload architectures:"
lipo -archs "$OFX_BIN" || true

# 1) Component package: the raw payload + where it installs.
COMPONENT_PKG="$SCRATCH/Maske-component.pkg"
pkgbuild \
    --root "$STAGING" \
    --install-location "$INSTALL_LOCATION" \
    --identifier "$IDENTIFIER" \
    --version "$VERSION" \
    "$COMPONENT_PKG"

# 2) Product archive: adds title, license, and a friendly install UI.
DIST_XML="$SCRATCH/distribution.xml"
cat > "$DIST_XML" <<XML
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>Maske ${VERSION}</title>
    <welcome    file="welcome.html"    mime-type="text/html"/>
    <license    file="LICENSE.txt"     mime-type="text/plain"/>
    <conclusion file="conclusion.html" mime-type="text/html"/>
    <options customize="never" require-scripts="false" hostArchitectures="arm64,x86_64"/>
    <domains enable_localSystem="true"/>
    <pkg-ref id="${IDENTIFIER}"/>
    <choices-outline>
        <line choice="default"/>
    </choices-outline>
    <choice id="default" title="Maske">
        <pkg-ref id="${IDENTIFIER}"/>
    </choice>
    <pkg-ref id="${IDENTIFIER}" version="${VERSION}" onConclusion="none">Maske-component.pkg</pkg-ref>
</installer-gui-script>
XML

RES_DIR="$SCRATCH/resources"
mkdir -p "$RES_DIR"
cp "$REPO_ROOT/LICENSE" "$RES_DIR/LICENSE.txt"
cat > "$RES_DIR/welcome.html" <<'HTML'
<html><body style="font-family:-apple-system,Helvetica,Arial;font-size:13px;line-height:1.5">
<p><b>Maske</b> installs a history-brush matte painting plugin into DaVinci Resolve's
Color page.</p>
<p>This installer copies the plugin into <code>/Library/OFX/Plugins</code>. You will be
asked for your administrator password.</p>
<p><b>Quit DaVinci Resolve before continuing</b> — it only scans for plugins at launch.</p>
</body></html>
HTML
cat > "$RES_DIR/conclusion.html" <<'HTML'
<html><body style="font-family:-apple-system,Helvetica,Arial;font-size:13px;line-height:1.5">
<p><b>Done.</b> Launch DaVinci Resolve Studio, open the Color page, and find the plugin
under <b>OpenFX → Mustafa Ekinci → Maske</b>.</p>
<p>DaVinci Resolve <b>Studio</b> is required — the free edition does not load third-party
OpenFX plugins.</p>
</body></html>
HTML

FINAL_PKG="$OUT_DIR/Maske-${VERSION}-macOS.pkg"
PB_ARGS=(
    --distribution "$DIST_XML"
    --package-path "$SCRATCH"
    --resources "$RES_DIR"
)
if [[ -n "${INSTALLER_SIGN_ID:-}" ]]; then
    PB_ARGS+=(--sign "$INSTALLER_SIGN_ID")
    echo "==> Signing installer with: $INSTALLER_SIGN_ID"
else
    echo "==> Building UNSIGNED installer (fine for local testing;"
    echo "    set INSTALLER_SIGN_ID + notarize for public distribution)."
fi
productbuild "${PB_ARGS[@]}" "$FINAL_PKG"

echo ""
echo "==> Installer written to: $FINAL_PKG"
pkgutil --check-signature "$FINAL_PKG" 2>/dev/null | head -3 || true
