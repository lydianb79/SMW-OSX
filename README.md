# SMW Recomp

A native macOS app that runs your own Super Mario World ROM (`smw.smc`), rendered with Metal.

The cartridge's code runs on a C core written from scratch for this project: a 65C816 CPU, an S-PPU
for graphics, an SPC700 + S-DSP for sound, and DMA/HDMA. No game assets ship with the app; everything
is read from your ROM at launch.

This is an interpreter, not a static recompilation. It runs the ROM's original code; nothing has been
rewritten in C.

## Requirements

- macOS 13 or later
- Xcode with Swift 5.9 or later
- Optional: the Metal toolchain (`xcodebuild -downloadComponent MetalToolchain`). If it's installed,
  `make` precompiles the shaders; otherwise the app compiles them at launch.

## Build and run

```bash
make
```

```bash
make run
```

```bash
make test
```

- `make` builds `build/SMWRecomp.app`. If `smw.smc` is in the project root, it is copied into the app.
- `make run` builds and opens the app.
- `make test` runs 600 frames headless and saves screenshots to `build/`.
- `make clean` deletes `build/`.

On launch, the app opens the last ROM you used, then the ROM bundled in the app. If it finds neither, it
asks you to pick one. Game → Open ROM… (⌘O) loads a ROM from anywhere.

## Menus

| Menu | Items |
| --- | --- |
| SMW Recomp | About, Settings… (⌘,), Quit |
| Game | Open ROM… (⌘O), Pause (⌘P), Reset (⌘R), Power Cycle (⇧⌘R), Close Window (⌘W) |
| State | Save State (⌘S), Load State (⌘L), Slot 1–9 (⌘1–⌘9), Save State As… (⇧⌘S), Load State From… (⇧⌘L) |
| View | Enter Full Screen (⌃⌘F) |

Each entry in the slot menu shows when that slot was last saved.

## Settings

### Input

You can rebind every SNES button for both the keyboard and any GameController-compatible pad. Click a
binding, then press the key or pad button to assign it; press Esc to clear it. The D-pad and left stick
always steer.

Default keyboard layout:

| SNES | Key | SNES | Key |
| --- | --- | --- | --- |
| D-pad | Arrow keys | Start | Return |
| B (jump) | X | Select | Right Shift |
| A (spin jump) | C | L / R | Q / W |
| Y (run / fire) | Z | Fast forward (hold) | Tab |
| X (run / fire) | S | Quick save / load | F5 / F9 |

Key names follow your keyboard layout. On a QWERTZ keyboard, for example, Y is shown where QWERTY has Z.

### Graphics

**Display**

- **Filter:** Nearest, Bilinear, Sharp Bilinear, Bicubic (Catmull-Rom), Lanczos, Smooth Edges (rounds
  pixel-art corners), LCD Grid, or Composite Video (soft picture with color bleed).
- **Aspect ratio:** 4:3 (original TV), 8:7 (square pixels), or stretch to window.
- **Integer scaling.**

**Full Screen**

- **Start in full screen:** the app opens straight into full screen.
- **Full screen resolution:** keep your desktop resolution (the default), or pick one of your display's
  modes. The display switches only while the game is full screen. It returns to your desktop resolution
  when you leave full screen or quit, and macOS restores it if the app crashes.

**Scanlines & CRT Mask** (applied on top of any filter, including the modern mode)

- **Presets:** Clean, Arcade Monitor, Consumer TV, LCD Handheld.
- **Scanlines:** Off, Sharp lines, or Soft CRT beam (bright areas glow wider), with intensity and line
  thickness sliders.
- **Phosphor mask:** None, Aperture grille, Slot mask, or Shadow mask (dots), with a strength slider.
- **Brightness compensation:** offsets the darkening from lines and masks.

Masks and thin lines look best at 4× scale or more.

**Modern**

- **Modernized 2026 Graphics:** smooth upscaling of the pixel art, glow on bright areas, filmic color
  grading, and a blurred, dimmed copy of the game filling the border instead of black bars. While it's
  on, it replaces the filter you picked.
- Sliders for bloom and color vibrance, and a switch for the ambient border fill.

### Audio

- Volume and mute.
- **Modernized 2026 Audio:** crisper cubic sample interpolation instead of the console's muffled Gaussian
  smoothing, then bass and treble shelving, stereo widening, reverb and a soft limiter. Reverb and stereo
  width have sliders.

## Saves

- **Battery save:** the in-game save is written to `~/Library/Application Support/SMW Recomp/Saves`
  every few seconds and when you quit.
- **Save states:** stored in `~/Library/Application Support/SMW Recomp/States`. Each state is tied to its
  ROM, so a state from a different ROM won't load.

## Project layout

| Path | Contents |
| --- | --- |
| `src/core/snes.c` | Bus, timing, DMA/HDMA, I/O registers, cartridge mapping, save states |
| `src/core/cpu.c` | 65C816 CPU |
| `src/core/ppu.c` | Graphics: registers and scanline renderer |
| `src/core/apu.c` | Sound: SPC700 CPU and S-DSP |
| `src/app/main.swift` | App delegate, window, menus, keyboard handling |
| `src/app/Emulator.swift` | Frame loop, audio engine, battery saves, save states |
| `src/app/Renderer.swift` | Metal renderer |
| `src/app/Settings.swift`, `SettingsView.swift` | Settings model and the Settings window |
| `src/app/Input.swift` | Keyboard and gamepad input |
| `src/app/DisplayModes.swift` | Full-screen resolution list and switching |
| `src/app/audio_out.c` | Audio ring buffer, resampler and modern audio effects |
| `src/shaders/Shaders.metal` | Filters, scanlines/masks and modern-mode shaders |
| `tools/make_icon.swift` | Draws the app icon |
| `tools/smwtest.c` | Headless test runner |

## Known limitations

- Hi-res modes 5/6 are approximated and offset-per-tile modes are not implemented. Super Mario World uses neither.
- Mode 7 scenes (boss fights) haven't been checked in detail yet.

Don't distribute a build that has the ROM bundled.
