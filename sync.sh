#!/bin/sh
# Copy the rig's JSFX from the LMS repo (default ~/LMS) into jsfx/
src="${1:-$HOME/LMS}"
cd "$(dirname "$0")/jsfx" || exit 1
cp "$src"/lms_*_v2.jsfx "$src/lms_satans_pedalboard.jsfx" "$src/lms_core.jsfx-inc" "$src/shakebot_logo.png" .
