#!/bin/sh
# Portable wrapper around the pinned profile in RocketLogger/sketch.yaml.
set -eu
project_dir=$(dirname "$0")/..
cd "$project_dir"
action=${1:-build}
port=${2:-}
if [ "$action" != build ] && [ "$action" != upload ]; then
  echo 'Usage: tools/build.sh [build | upload SERIAL_PORT]' >&2
  exit 2
fi
if [ "$action" = upload ] && [ -z "$port" ]; then
  echo 'Specify the device port: tools/build.sh upload SERIAL_PORT' >&2
  exit 2
fi
cli=${ROCKET_ARDUINO_CLI:-arduino-cli}
if ! command -v "$cli" >/dev/null 2>&1; then
  if [ -x '/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli' ]; then
    cli='/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli'
  else
    echo 'Install Arduino CLI or set ROCKET_ARDUINO_CLI to its executable path.' >&2
    exit 1
  fi
fi
set --
# Work around the Intel-only bundled ctags on the development ARM Mac.
if [ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] && [ -x /opt/homebrew/bin/ctags ]; then
  set -- --build-property runtime.tools.ctags.path=/opt/homebrew/bin
fi
"$cli" compile --profile cwa_v2 "$@" --build-path build/profile RocketLogger
if [ "$action" = upload ]; then
  "$cli" upload --profile cwa_v2 --port "$port" --input-dir build/profile RocketLogger
fi
