# n64 — shared N64 platform library for the PocketGo S30 ports

Used by `ports/oot`, `ports/mm`, `ports/sf64`, `ports/mk64` and `ports/ssb` (each
`Makefile.s30` includes `n64.mk` and links `$(N64LIB_OBJS)`). Game-independent: no game headers.

| File | Header | What |
|---|---|---|
| `src/display.c` | `s30_display.h` | KMS/GBM/EGL on the DRM lease, landscape framebuffer rotated onto the portrait panel, 2D layer, frame pacing, refresh mode (`S30Display_SetRefresh`), screenshots, motion blur / pause background / VisMono helpers |
| `src/gfx.c` | `s30_gfx.h` | The RSP/RDP renderer (GLES2): F3DEX2 + S2DEX, F3DEX translated to F3DEX2 (`f3dex_to_f3dex2`), TMEM, combiner shaders, texture cache, replacement textures, depth probes |
| `src/audio.c` | `s30_audio.h` | ALSA output; the game's audio update runs on its thread (`S30Audio_Init(update)`); `S30Audio_Configure`: the game's sample rate, clock-paced updates for AI-length buffer sizing |
| `src/mixer.c` | `s30_mixer.h` | The audio microcode (aspMain) in software; each game redirects its own Acmd macros to the `a*Impl` functions (Mario Kart's variants: `aEnvMixerEuImpl`, `aLoadBuffer8Impl`, ...) |
| `src/input.c` | `s30_input.h` | The S30 gamepad as an N64 controller, with the game's button map |
| `src/app.c` | `s30_app.h` | Crash reports (`crash.log`; `kill -QUIT` prints where each N64 thread is), fatal errors, the save path (`$XDG_DATA_HOME/<game>/`) |
| `src/os.c` | `s30_os.h` | libultra's OS: threads (one N64 thread on the CPU at a time, by priority), message queues and events, time and timers, VI retrace, the RCP task queue (`S30Os_RunRcp` on the main thread), controllers, a Controller Pak in controller 1 (`pakPath`: a 32 KB image; libultra's own pak code talks to an emulated PIF, `__osSiRawStartDma`), no Rumble Pak or Voice Recognition Unit, PI DMA from a ROM image, EEPROM/SRAM saves (written whole, fsync'd, only when they change), the AI. Opt-in: `N64LIB_OS := 1`, used by every port; a port that builds a libultra piece of its own (`_Printf`, `__sinf`, its `osAiSetNextBuffer`) overrides the weak one here |

A game configures the renderer once (`S30Gfx_Configure`): microcode, its
segment table, `Mtx` format (float or s15.16), how its texture data is stored
(u64 words, or a callback giving each array's element size) and the prefix of
its debug variables (`<PREFIX>_GFXSTATS`, `_GFXTRACE`, `_SHOT`, `_REFRESH`,
`_REFRESHTEST`, `_NOAUDIO`, ...).

## Porting checklist (little-endian, 32-bit pointers at 0x40000000+)

Bugs every N64 port here has hit. Check each in a new game before its first run:

- **Address tests:** `ptr < 0x7FFFFFFF` / `>= K0BASE` (KSEG0) checks, e.g. the
  audio's `AUDIO_RELOCATED_ADDRESS_START` and "is this note pointer valid":
  real pointers here are 0x40000000 and up.
- **K0/physical conversions** (`K0_TO_PHYS`, `OS_K0_TO_PHYSICAL`, ...): make
  them identity, or resolve them through the segment table.
- **Unions that pack bytes into words:** `AudioCmd` (`op`/`arg0..2` and
  `asSbyte`/`asUbyte`/`asUShort` views) read the wrong byte on little-endian.
- **Bitfields** read from N64 data or written as a byte (`Sample`, `StereoData`):
  list them in reverse under the little-endian build.
- **Big-endian data loaded from the ROM:** sound fonts, samples, ADPCM books and
  loops, envelopes, the sequence/font table; swap at load (track shared
  structures so they are swapped once) or at read.
- **libultra code that takes floats/doubles apart** (sinf/cosf): use the C
  library's.
- **`Mtx`:** the game's own code and `guMtxF2L` must agree on where the
  integer and fraction halves go (`u.i`/`u.f`).
- **Negative opcode constants** (F3DEX `G_IMMFIRST - n`): `switch` on a `u8`
  needs `case (u8)G_X`.
- **Single-threaded waits:** anything that blocks on a message another N64
  thread would send (save, timers, controller) needs that thread or a direct
  call.
- **Segment 0 as KSEG0:** code that reaches RAM as `(addr & 0xFFFFFF) +
  segment 0` (Mario Kart) needs segment 0 at the image base and the segment
  number masked to 4 bits.
- **Audio rate and pacing:** check the game's output frequency (Mario Kart:
  26800 Hz) and whether it sizes buffers from `osAiGetLength`; if so, use
  `S30Audio_Configure` (paced updates) or tempo and pitch drift.
- **Microcode semantics:** each game's audio microcode differs (envelope
  mixer, DMA rounding); check the Acmd mapping against mupen64plus-rsp-hle's
  alist for that game. ABI1 (libultra 2.0 aspMain) comes in variants told
  apart by the ucode data word at +0x28 (rsp-hle's `hle.c`): the common one
  ramps envelope volumes exponentially, Diddy Kong Racing / Blast Corps
  (0x1e3c1390) and GoldenEye linearly (`alist_envmix_ge`); the wrong ramp
  plays the first sequence and leaves later ones silent or stuck.
- **Shared sound-bank structures:** envelopes, ADPCM books and loops are
  shared between sounds; swap each once (a set of seen pointers that cannot
  fill up: DKR's two bank files hold more than a thousand), or a second visit
  swaps them back and release times become 20-minute ramps.
- **CPU-written textures:** RAM is read in N64 byte order; code that builds
  or recolours pixels/palettes with native u16 stores must swap.
