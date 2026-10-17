#!/bin/sh
# Install LMS Rig for the current user: app, VST3, menu entry.
# Looks for the build next to this script first (release zip), then in ~/ysfx/build.
set -e

here="$(cd "$(dirname "$0")" && pwd)"
if [ -x "$here/LMS Rig" ]; then
    app="$here/LMS Rig"; vst="$here/LMS Rig.vst3"
else
    art="${YSFX_BUILD:-$HOME/ysfx/build}/lms_rig_artefacts/Release"
    app="$art/Standalone/LMS Rig"; vst="$art/VST3/LMS Rig.vst3"
fi
[ -x "$app" ] || { echo "LMS Rig binary not found (looked in $here and ~/ysfx/build)"; exit 1; }

prefix="$HOME/.local/opt/lms-rig"
mkdir -p "$prefix" "$HOME/.local/bin" "$HOME/.local/share/applications" "$HOME/.local/share/icons/hicolor/256x256/apps"

rm -f "$prefix/lms-rig"   # unlink first so a running copy doesn't block the update
cp "$app" "$prefix/lms-rig"
ln -sf "$prefix/lms-rig" "$HOME/.local/bin/lms-rig"
cp "$here/icon.png" "$HOME/.local/share/icons/hicolor/256x256/apps/lms-rig.png"

if [ -d "$vst" ]; then
    mkdir -p "$HOME/.vst3"
    rm -rf "$HOME/.vst3/LMS Rig.vst3"
    cp -R "$vst" "$HOME/.vst3/"
fi

# JUCE's ALSA backend can crash on first launch; start on JACK (PipeWire provides it)
settings="$HOME/.config/LMS Rig.settings"
if [ ! -f "$settings" ]; then
    cat > "$settings" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>

<PROPERTIES>
  <VALUE name="audioSetup">
    <DEVICESETUP deviceType="JACK"/>
  </VALUE>
  <VALUE name="shouldMuteInput" val="0"/>
</PROPERTIES>
EOF
fi

cat > "$HOME/.local/share/applications/lms-rig.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=LMS Rig
Comment=Satan's Little Pedal Board into LMS amp heads
Exec="$prefix/lms-rig"
Icon=lms-rig
Terminal=false
Categories=AudioVideo;Audio;Music;
EOF

update-desktop-database "$HOME/.local/share/applications" 2>/dev/null || true
echo "Installed: app menu 'LMS Rig', command 'lms-rig'$( [ -d "$vst" ] && echo ', VST3 in ~/.vst3')"
