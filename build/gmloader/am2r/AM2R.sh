#!/bin/sh
# AM2R (Another Metroid 2 Remake): the Android build (AM2RLauncher's APK) run
# by gmloader-next in /mnt/sd/home/ports/am2r (am2r.apk, gmloader.json,
# lib/armeabi-v7a); saves in saves/. Optional debug environment in am2r.env
# (VAR=value lines).
#
# The S30's face buttons by position, as the game expects an Xbox pad: bottom
# (B, BTN_SOUTH) -> a, right (A, BTN_EAST) -> b, left (Y, BTN_WEST) -> x, top
# (X, BTN_NORTH) -> y. SDL's own guess swaps the left and top buttons.
export SDL_GAMECONTROLLERCONFIG="18007540506f636b6574476f20533300,PocketGo S30 Gamepad,a:b0,b:b1,x:b3,y:b2,back:b8,start:b9,leftstick:b10,leftshoulder:b4,rightshoulder:b5,dpup:b11,dpdown:b12,dpleft:b13,dpright:b14,leftx:a0,lefty:a1,lefttrigger:b6,righttrigger:b7,platform:Linux,"
[ -f /mnt/sd/home/ports/am2r/am2r.env ] && set -a && . /mnt/sd/home/ports/am2r/am2r.env && set +a
cd /mnt/sd/home/ports/am2r && exec ./gmloadernext.armhf -c gmloader.json
