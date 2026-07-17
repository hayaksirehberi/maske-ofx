# Installers

Double-clickable installers for **Maske**, so end users never touch the OFX
plugin folder by hand.

| Platform | Deliverable | Installs to |
|---|---|---|
| macOS | `dist/Maske-1.2-macOS.pkg` | `/Library/OFX/Plugins/HistoryBrush.ofx.bundle` |
| Windows | `dist/Maske-1.2-Windows-Setup.exe` (Inno Setup) **or** the `install.bat` ZIP | `C:\Program Files\Common Files\OFX\Plugins\HistoryBrush.ofx.bundle` |

Both ask for admin rights through the OS (Installer UI / UAC) — the user just
clicks through. **DaVinci Resolve Studio** is required, and it must be quit
during install (it only scans for plugins at launch).

---

## macOS — `.pkg`

Prereqs: a built `build/HistoryBrush.ofx.bundle` (universal arm64 + x86_64) and
Xcode command line tools (for `pkgbuild`/`productbuild`, already on most Macs).

```sh
installer/macos/build_pkg.sh
# -> dist/Maske-1.2-macOS.pkg
```

The user double-clicks the `.pkg` and follows the wizard.

### Signing & notarization (for distribution to other Macs)

The unsigned `.pkg` is fine for your own machine, but on someone else's Mac
Gatekeeper will block it (right-click → Open is the manual workaround). For a
clean public download you need an Apple Developer ID:

```sh
# Sign the installer:
INSTALLER_SIGN_ID="Developer ID Installer: Your Name (TEAMID)" \
    installer/macos/build_pkg.sh

# The .ofx binary inside should also be Developer ID-signed and the whole
# thing notarized:
codesign --force --options runtime --timestamp \
    --sign "Developer ID Application: Your Name (TEAMID)" \
    build/HistoryBrush.ofx.bundle/Contents/MacOS/HistoryBrush.ofx
# (rebuild the pkg, then:)
xcrun notarytool submit dist/Maske-1.2-macOS.pkg --keychain-profile "AC_PASSWORD" --wait
xcrun stapler staple dist/Maske-1.2-macOS.pkg
```

---

## Windows — `Setup.exe` (recommended) or `install.bat`

The Windows plugin must be built first (`build\HistoryBrush.ofx.bundle`, with
the CUDA path — see the repo README).

### Option A — Inno Setup (single Setup.exe)

1. Install [Inno Setup 6](https://jrsoftware.org/isdl.php).
2. Compile:
   ```bat
   "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\windows\Maske.iss
   ```
   Output: `dist\Maske-1.2-Windows-Setup.exe`. Ship that one file.

Unsigned installers trigger a SmartScreen "unknown publisher" prompt; sign the
`.exe` with a code-signing certificate to avoid it.

### Option B — no toolchain (`install.bat`)

For a quick release without Inno Setup: put `install.bat`, `uninstall.bat`, and
the built `HistoryBrush.ofx.bundle` folder together in a ZIP. The user extracts
and double-clicks `install.bat` — it self-elevates (UAC) and copies the plugin
into place. `uninstall.bat` removes it.
