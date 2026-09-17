#!/bin/sh
# Creates the release signing key outside the project, with a random password,
# at ~/.config/tinyspot/. Keep a backup: releases signed with a different key
# cannot upgrade an installed app.
set -e
DIR="$HOME/.config/tinyspot"
mkdir -p "$DIR"; chmod 700 "$DIR"
[ -f "$DIR/tinyspot.jks" ] && { echo "keystore already exists at $DIR"; exit 0; }
PW=$(head -c 24 /dev/urandom | base64 | tr -d '/+=' | head -c 28)
keytool -genkeypair -keystore "$DIR/tinyspot.jks" -alias tinyspot -keyalg RSA \
  -keysize 4096 -validity 10950 -storepass "$PW" -keypass "$PW" \
  -dname "CN=TinySpot, O=$(id -un), C=ES"
printf 'storeFile=%s/tinyspot.jks\nstorePassword=%s\nkeyAlias=tinyspot\nkeyPassword=%s\n' \
  "$DIR" "$PW" "$PW" > "$DIR/keystore.properties"
chmod 600 "$DIR/keystore.properties" "$DIR/tinyspot.jks"
echo "created $DIR/tinyspot.jks"
