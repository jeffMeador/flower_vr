#!/bin/bash
# Launch Flower (GOG) with Proton outside the Steam UI, VR mod enabled.
# Copy to ~/Games on the Frame. Usage: ~/Games/run_flower.sh (the GOG game in ~/Games/Flower_GOG).
S="$HOME/.local/share/Steam"
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$S"
export STEAM_COMPAT_DATA_PATH="$HOME/Games/Flower_pfx"
export SteamAppId=0 SteamGameId=0
export DISPLAY=:0 XDG_RUNTIME_DIR=/run/user/$(id -u)
export WINEDLLOVERRIDES="d3d11=n,b"
export PROTON_LOG=${PROTON_LOG:-0} PROTON_LOG_DIR="$HOME/Games"  # PROTON_LOG=1 for a Wine log
mkdir -p "$STEAM_COMPAT_DATA_PATH"
cd "$HOME/Games/Flower_GOG"
exec "$S/steamapps/common/SteamLinuxRuntime_4-arm64/_v2-entry-point" --verb=waitforexitandrun -- \
  "$S/steamapps/common/Proton 11.0 (ARM64)/proton" waitforexitandrun "$HOME/Games/Flower_GOG/Flower.exe"
