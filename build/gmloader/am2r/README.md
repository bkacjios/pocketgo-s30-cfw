# AM2R on the PocketGo S30

AM2R (Another Metroid 2 Remake) 1.5.5, its Android build, run natively by
gmloader-next (`build.sh gmloader`, patched for the S30 in
`patches/gmloader-next`: musl, bionic armv7's 32-bit ABI, the host libm, the
portrait panel).

The game itself isn't here: build its APK from your copy of the AM2R 1.1 zip
with the community's AM2RLauncher (Android, "Community Updates").

## Install

On the card, `/mnt/sd/home/ports/am2r/`:

    gmloadernext.armhf           s30-build/gmloader/
    lib/armeabi-v7a/*.so         s30-build/gmloader/lib/armeabi-v7a/
    gmloader.json                this directory (build/gmloader/am2r)
    config-s30.ini               this directory
    am2r.apk                     AM2RLauncher's APK, renamed
    saves/                       created by the game

and `AM2R.sh` (this directory) in `/mnt/sd/apps/` for the launcher.

## Controls

`AM2R.sh` maps the face buttons by position for SDL (the game expects an Xbox
pad). `gmloader.json` puts the pad in gamepad slot 1, the one AM2R reads
(`first_pad_slot`). On the first run (no `saves/config.ini` yet) `AM2R.sh`
copies `config-s30.ini` there: the Game Boy layout (A, right, jumps and
confirms; B, bottom, shoots and cancels; Y, left, aims) and the SNES button
prompts (`[Extras] Controller="3.000000"`), which show the S30's labels. The
game's own options menu changes them from then on.

## Debugging

`am2r.env` next to the APK (VAR=value lines): `S30_GML_INPUT_LOG=1` logs the
pad and the game's gamepad calls; `S30_GML_KEEP_SEGV=1` keeps the loader's
crash report (module + offset) instead of the game's own handlers. Output in
`/tmp/frontend.log`.
