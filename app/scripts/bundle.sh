#!/usr/bin/env bash
# Builds LocalASR.app into dist/: the Swift app with asrd beside it in Contents/MacOS.
#
# Signs with the first "Apple Development" identity in the keychain when there is one, so that the Microphone and
# Accessibility permissions survive rebuilds; ad-hoc signing changes the app's identity on every build and macOS
# then asks again. CODESIGN_IDENTITY overrides the choice ("-" for ad hoc). A "Developer ID Application" identity, for an
# app given to others, also gets the hardened runtime with the microphone entitlement, as notarization requires.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
APP="$ROOT/dist/LocalASR.app"

swift build -c release --package-path "$ROOT/app"
BIN="$(swift build -c release --package-path "$ROOT/app" --show-bin-path)/LocalASR"
[[ -x "$ROOT/build/asrd" ]] || { echo "build/asrd is missing; run make server" >&2; exit 1; }

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$ROOT/app/Resources/Info.plist" "$APP/Contents/Info.plist"
cp "$BIN" "$APP/Contents/MacOS/LocalASR"
cp "$ROOT/build/asrd" "$APP/Contents/MacOS/asrd"

IDENTITY="${CODESIGN_IDENTITY:-$(security find-identity -v -p codesigning | awk -F'"' '/Apple Development/ {print $2; exit}')}"
IDENTITY="${IDENTITY:--}"
FLAGS=(--force --sign "$IDENTITY")
[[ $IDENTITY == "Developer ID Application"* ]] && FLAGS+=(--options runtime --timestamp)
codesign "${FLAGS[@]}" "$APP/Contents/MacOS/asrd"
codesign "${FLAGS[@]}" --entitlements "$ROOT/app/Resources/LocalASR.entitlements" "$APP"
echo "built $APP (signed: $([[ $IDENTITY == - ]] && echo ad-hoc || echo "$IDENTITY" | sed 's/:.*//'))"
