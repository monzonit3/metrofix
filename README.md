# metrofix
This patch provides some fixes for the native Linux versions of Metro Redux (2033 & Last Light).

## Enhancements
 - Allow for newer dynamic SDL versions for native Wayland/Pipewire support (should fix many windowing/audio problems)
 - Better gamepad mapping support (provided by SDL_GameController instead of the raw joystick api)
 - Gamepad vibration support
 - Allow to use different resolutions from the monitor's one through the `METRO_RESOLUTION_OVERRIDE` environment variable
 - may fix crashes with some gl drivers 
 
## Install
To apply the fixes download the tarball from Releases and then extract the contents inside the game's directory, then launch through `metrofix.sh`

## Troubleshooting
If you are experiencing crashes at startup try launching the Steam client first and only then open the game from the script.

## Building
To compile on a Linux host just use `make`, mind you it has only been tested using gcc on Debian 13 and it might not produce working binaries on different configurations

## AI Disclosure
LLMs have been used to aid the creation of this project
