#!/bin/sh
export SDL_DYNAMIC_API=./metrofix.so
exec ./metro "$@"
