# Doom64-PS3

**Doom 64 for the PlayStation 3** (homebrew, PSL1GHT).

Doom64-PS3 is a PS3 port of [doom64-dc](https://github.com/jnmartin84/doom64-dc),
jnmartin84's Dreamcast port of Doom 64, which in turn is built on
[DOOM64-RE](https://github.com/Erick194/DOOM64-RE) (Erick194's reverse-engineered
Nintendo 64 source) and Immorpher's Merciless Edition / XE. It runs the original
game code natively on the PS3's Cell CPU and RSX GPU — it is not an emulator.

It ships **no game data**. Everything comes from your own copy of the game.

---

## What you need

| File (copy to `/dev_hdd0/game/DOOM64CEL/USRDIR/`) | Required | What it is |
|---|---|---|
| Doom 64 N64 ROM (`.z64`, `.v64` or `.n64`, any name) | **yes**, first boot | USA version 1.0 or 1.1, **unzipped** |
| `DOOM64.WAD` from the 2020 Nightdive remaster | no | adds **The Lost Levels** (MAP34–40) |

On first boot the game converts the ROM (graphics, maps, music and sound
effects) into its own files. This takes one or two minutes and happens only
once; a progress screen is shown. If the remaster's `DOOM64.WAD` is present,
the Lost Levels are converted too (a few seconds).

After the conversion **you can delete both the ROM and `DOOM64.WAD`** — the game
never opens them again. Keep a copy on your PC: you will need them again only
if the converted files are lost (for example, if you uninstall the game from
the XMB, which deletes `USRDIR`). Installing a newer PKG over an existing
install keeps the converted files.

### Optional: Knee-Deep in the Dead

doom64-dc includes a bonus Knee-Deep in the Dead episode (maps by z0k, music by
Andrew Hulshult). It is not included here. If you want it, copy these files from
the doom64-dc repository into `USRDIR`:

- `maps/map41.wad` … `maps/map49.wad`
- `doom1mn.lmp`
- `mus/e1m1.adpcm` … `mus/e1m9.adpcm` (its music; without them, N64 music plays)

The "Choose Campaign" menu shows whichever extra episodes are installed.

---

## Controls (DualShock 3)

Two layouts, selectable under **Options → Gamepad**:

| Action | Classic | Modern |
|---|---|---|
| Fire | R2 / Square | R2 |
| Use / open | Cross | Square / Cross |
| Previous / next weapon | L1 / R1 | L1 / R1 (Triangle: next) |
| Automap | Select | Select |
| Walk / run | L2 (hold) | L3 (toggle) |
| Move / strafe | Left stick | Left stick |
| Turn | Right stick | Right stick |
| D-pad | same as the N64 D-pad | same |
| Pause menu | Start | Start |

In menus: D-pad or left stick to move, left/right to change a value, Cross to
select, Circle or Start to go back.

The Gamepad screen also has the stick deadzone, with a live stick readout.
Cheats are in the **Features** menu.

---

## Saves

Like on the N64, the game saves your progress **when you finish a level** (and
you can always use passwords). Saves and settings live in
`/dev_hdd0/data/doom64/` (`doom64.sav` and `doom64.stg`). To back them up,
copy that folder over FTP or to a USB drive; delete it to start from scratch.

---

## Building

Requires the [ps3toolchain](https://github.com/ps3dev/ps3toolchain) and
[PSL1GHT](https://github.com/ps3dev/PSL1GHT).

```bash
export PS3DEV=/usr/local/ps3dev
export PSL1GHT=$PS3DEV
export PATH=$PS3DEV/bin:$PS3DEV/ppu/bin:$PATH

git clone <this repository URL> Doom64-PS3
cd Doom64-PS3
make check-toolchain
make clean && make
```

This produces `doom64-ps3-1.0.pkg`. It appears in the XMB as **Doom 64**
(title ID `DOOM64CEL`).

`make clean && make DEBUG=1` builds `doom64debug.pkg` instead: the same game
plus test buttons in Options → Gamepad (L3 god mode, R3 all weapons/keys,
Triangle exit level; in the Modern layout, hold L2 and press those). Both
builds share the title ID, so installing one replaces the other.

If something goes wrong, the log is at
`/dev_hdd0/game/DOOM64CEL/USRDIR/doom64_log.txt`.

---

## How it works (short version)

- `source/engine/` — the doom64-dc game code (DOOM64-RE based), almost untouched.
- `source/compat/` — a KallistiOS / PowerVR compatibility layer: the engine
  still "talks Dreamcast", and this layer turns that into PS3 calls (RSX
  rendering, audio, pad, files).
- `source/romconv/` — doom64-dc's PC-side `wadtool`, adapted to run on the
  console (big-endian) and convert your ROM on first boot.
- `source/wess/` — the original N64 sound driver (Williams' WESS, from
  DOOM64-RE) plus a C re-implementation of the N64 audio synthesizer, so music
  and sound effects play exactly as on the N64, straight from the ROM.

---

## Credits

- **doom64-dc** — jnmartin84 (the Dreamcast port this is based on), with thanks
  to its contributors: Falco Girgis, Paul Cercueil, Quzar, SWAT, BBHoodsta,
  Kazade, Erik5249, Lobotomy, Mittens, StrikerTheHedgefox, the
  #kallistios / Simulant Discord and every tester
- **DOOM64-RE** and **D64TOOL** — Erick194 (reverse-engineered Doom 64 and the
  data tools the ROM converter descends from)
- **Merciless Edition / Doom 64 XE** — Immorpher
- **Knee-Deep in the Dead** for Doom 64 — maps by z0k, music by Andrew Hulshult
- RSX/GCM init patterns and precompiled shaders from
  [IoQuake3-PS3](https://github.com/Mayo1970/IoQuake3-PS3) (Mayo1970)
- [PSL1GHT](https://github.com/ps3dev/PSL1GHT) and the
  [ps3toolchain](https://github.com/ps3dev/ps3toolchain) — the ps3dev team
- **Doom 64** — Midway Studios San Diego and id Software; music and sound design
  by Aubrey Hodges. Doom 64 © id Software. This project is not affiliated with
  or endorsed by id Software, Bethesda, Midway or Nightdive Studios.

## License

GPLv3 (see `LICENSE`), like DOOM64-RE and doom64-dc. No game data is included
or distributed.
