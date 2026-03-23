#!/bin/sh
cd "$(dirname -- "$0")"
export SDL_DYNAMIC_API=./metrofix.so
#export METRO_RESOLUTION_OVERRIDE=1280x720
exec ./metro "$@"
