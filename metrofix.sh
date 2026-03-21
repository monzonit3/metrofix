#!/bin/sh
cd "$(dirname -- "$0")"
export SDL_DYNAMIC_API=./metrofix.so
exec ./metro "$@"
