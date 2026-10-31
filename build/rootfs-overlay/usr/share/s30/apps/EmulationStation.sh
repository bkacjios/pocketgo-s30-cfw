#!/bin/sh
# EmulationStation (systems in /etc/emulationstation/es_systems.cfg; settings,
# input mapping and scraped data in ~/.emulationstation). The panel is
# portrait (320x480), mounted landscape: --screenrotate turns the UI onto
# it and --screensize lays it out landscape (480x320), not as the window.
exec emulationstation --screenrotate 3 --screensize 480 320
