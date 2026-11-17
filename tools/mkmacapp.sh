#!/bin/sh
# tools/mkmacapp.sh [version]: build/PiArcBoot.app from a Mac build in build/ (make, which puts
# SDL2.framework beside the program). The program's files go in its Resources, ROOGUI and SDL2
# in its Frameworks, and its icon is drawn at each of a Mac's icon sizes. It is signed ad hoc,
# for no one in particular, which an Apple silicon Mac needs before it runs a program at all.
set -e
cd "$(dirname "$0")/.."
VERSION=${1:-$(git describe --tags --always 2>/dev/null | sed 's/^v//')}
VERSION=${VERSION:-dev}
APP=build/PiArcBoot.app
SET=build/PiArcBoot.iconset

rm -rf "$APP" "$SET"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$APP/Contents/Frameworks" "$SET"
cp build/piarcboot "$APP/Contents/MacOS/"
cp build/libroogui.dylib "$APP/Contents/Frameworks/"
cp -R build/SDL2.framework "$APP/Contents/Frameworks/"
cp -R ROM311 icons presets shaders icon.png LICENSE build/LICENSE-ROOGUI "$APP/Contents/Resources/"

for s in 16 32 128 256 512; do
	build/piarcboot icon app "$SET/icon_${s}x${s}.png" $s
	build/piarcboot icon app "$SET/icon_${s}x${s}@2x.png" $((s * 2))
done
iconutil -c icns -o "$APP/Contents/Resources/PiArcBoot.icns" "$SET"
rm -rf "$SET"

cat > "$APP/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>en</string>
	<key>CFBundleExecutable</key>
	<string>piarcboot</string>
	<key>CFBundleIconFile</key>
	<string>PiArcBoot</string>
	<key>CFBundleIdentifier</key>
	<string>io.github.jameshansen.piarcboot</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>PiArcBoot</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleShortVersionString</key>
	<string>$VERSION</string>
	<key>CFBundleVersion</key>
	<string>$VERSION</string>
	<key>LSApplicationCategoryType</key>
	<string>public.app-category.games</string>
	<key>LSMinimumSystemVersion</key>
	<string>10.13</string>
	<key>NSHighResolutionCapable</key>
	<true/>
	<key>NSHumanReadableCopyright</key>
	<string>GNU General Public License v3.0</string>
</dict>
</plist>
EOF

codesign --force --deep --sign - "$APP"
echo "$APP $VERSION"
