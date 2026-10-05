#!/usr/bin/env bash
# One-time setup (Git Bash): a portable JDK 25 for Minecraft 26.3 in tools/, and the Python packages.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
if ! ls -d "$HERE"/jdk-25* >/dev/null 2>&1; then
	curl -sSL -o "$HERE/jdk25.zip" "https://api.adoptium.net/v3/binary/latest/25/ga/windows/x64/jdk/hotspot/normal/eclipse"
	unzip -q "$HERE/jdk25.zip" -d "$HERE" && rm "$HERE/jdk25.zip"
fi
"$(ls -d "$HERE"/jdk-25* | tail -1)/bin/java" -version
python -m pip install --quiet pyyaml numpy pillow websockets lz4
