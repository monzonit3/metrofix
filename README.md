# metrofix
This patch provides some fixes for the native Linux versions of Metro Redux (2033 & Last Light).

## Enhancements
 - Allow for newer dynamic SDL versions for native Wayland/Pipewire support (should fix many windowing/audio problems)
 - Better gamepad mapping support (provided by SDL_GameController instead of the raw joystick api)
 - Gamepad vibration support
 - Allow to use different resolutions from the monitor's one through the `METRO_RESOLUTION_OVERRIDE` environment variable

## Install
To apply the fixes download the tarball from Releases and then extract the contents inside the game's directory, then launch through `metrofix.sh`

## Troubleshooting
If you are experiencing crashes at startup try launching the Steam client first and only then open the game from the script.
