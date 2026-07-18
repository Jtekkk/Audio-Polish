# Audio Polish

A "make it sound finished" VST3 / Standalone audio plugin built with [JUCE 8](https://juce.com).
One **Polish** macro takes a track or mix from flat to finished, with full per-stage
control underneath when you want it.

## Signal chain

```
Input gain
  -> Tone        (low shelf + high shelf, with a Tilt fold-in)
  -> Drive       (asymmetric tanh saturation, up to 8x oversampled, DC-blocked)
  -> Glue        (program-dependent compression)
  -> Width       (mid/side widening, low end kept mono as width increases)
  -> Output gain
  -> Ceiling     (brick-wall limiter)
  -> Mix         (latency-compensated dry/wet)
```

The **Polish** macro pushes Drive, Glue, top-end "air" and Width together so a single
knob does the heavy lifting; the individual controls trim from there.

### DSP quality notes

- **Anti-aliased saturation** — the waveshaper runs inside a selectable (2×/4×/8×),
  linear-phase oversampled block, so the harmonics it generates don't fold back as
  aliasing. Switching the factor (or Off) re-prepares the chain safely off the audio
  thread and updates the reported latency.
- **True-peak-safe ceiling** — the Ceiling limiter runs inside its own fixed 4×
  oversampled block (always on, independent of the Drive oversampling setting), so
  it brick-walls the *reconstructed* waveform and catches inter-sample peaks a
  1× limiter would let through.
- **64-bit precision** — the entire chain is templated on sample type and runs in
  full double precision when the host requests it (`supportsDoublePrecisionProcessing`),
  falling back to 32-bit float otherwise.
- **Even-harmonic warmth** — the saturator is mildly asymmetric (tube-like) rather
  than a pure odd-harmonic `tanh`; a DC blocker removes the resulting offset.
- **Frequency-conscious width** — as Width goes past 100 %, the low end of the side
  signal is progressively collapsed to mono so the bass stays centred and phase-safe.
- **Latency reporting** — the oversamplers' combined latency is reported to the host,
  and both the dry/wet Mix path and Bypass are delay-compensated to stay sample-aligned.
- **Optional TPDF dither** — a triangular-PDF dither stage, scaled for 16- or 24-bit
  output, can be switched on for the final render/bounce (see Dither below).

### Metering

- **LUFS-S / LUFS-I** — short-term and (gated) integrated loudness, using the
  ITU-R BS.1770 K-weighting filter and the standard absolute/relative gating
  algorithm. This is a practical mixing/mastering aid, not a certified
  compliance meter. Click the LUFS-I readout to reset its history (e.g. at the
  start of a song).
- **True Peak** — the post-limiter peak measured in the same oversampled domain
  the ceiling limiter runs in, so it reflects genuine inter-sample peaks (dBTP).
- **Correlation** — a stereo phase-correlation meter (-1 out of phase, +1
  mono-safe) so Width changes can be judged for mono compatibility at a glance.
- **Loudness Match** — when enabled, toggling Bypass gain-compensates the dry
  passthrough to match Polish's recently-measured loudness, so A/B comparisons
  judge the processing itself rather than whichever side happens to be louder.

## Controls

| Control   | Range            | What it does                                            |
|-----------|------------------|---------------------------------------------------------|
| Input     | −24…+24 dB       | Trim going into the chain                                |
| Polish    | 0…100 %          | Master macro: drive + glue + air + width                |
| Low       | −12…+12 dB       | Low shelf (≈150 Hz)                                      |
| High      | −12…+12 dB       | High shelf (≈6 kHz)                                      |
| Tilt      | −6…+6 dB         | Tilts the spectrum (darker ↔ brighter)                  |
| Drive     | 0…100 %          | Harmonic saturation amount                              |
| Glue      | 0…100 %          | Compression amount (threshold + ratio + makeup)         |
| Width     | 0…200 %          | Stereo width (0 = mono, 100 = unchanged)                |
| Ceiling   | −12…0 dB         | Output limiter ceiling                                   |
| Output    | −24…+24 dB       | Final trim before the ceiling limiter                   |
| Mix       | 0…100 %          | Dry/wet blend (parallel "polish"), latency compensated  |
| Oversampling | Off / 2× / 4× / 8× | Anti-aliasing for the saturator (CPU vs quality)     |
| Dither    | On / Off, 16-bit / 24-bit | TPDF dither before the final output              |
| Loudness Match | —           | Gain-compensates Bypass so A/B compares fairly          |
| Bypass    | —                | Latency-compensated bypass                              |

## Presets

Six factory presets are exposed through the host's program/preset menu and the
in-plugin **Presets** selector (top-right). They set the sonic parameters only —
Oversampling and Bypass are left untouched.

| Preset         | Character                                              |
|----------------|--------------------------------------------------------|
| Init / Flat    | Everything neutral — a clean starting point            |
| Subtle Polish  | Gentle sheen and light glue                            |
| Glue Bus       | Bus compression with a touch of low-end weight         |
| Warm Master    | Saturated, rounded, slightly dark master tone          |
| Wide & Bright  | Airy top end and a wider image                         |
| Loud & Proud   | Pushed drive + glue with extra output                  |

### User presets

The **Save** button (top-right, next to the preset selector) saves the current
knob settings as a named preset. User presets appear in the same selector,
below a separator after the factory presets, and persist as XML files under:

- `~/Documents/Audio Polish/Presets` (macOS/Linux)
- `Documents\Audio Polish\Presets` (Windows)

## Validation

Every CI build runs [pluginval](https://github.com/Tracktion/pluginval) at
strictness level 10 against the built VST3 (load, state, parameters, threading,
bus layouts, editor, parameter fuzzing). A failure blocks the installer and any
release. To run it locally:

```bash
pluginval --strictness-level 10 "build/AudioPolish_artefacts/Release/VST3/Audio Polish.vst3"
```

## Building

Requires CMake ≥ 3.22 and a C++17 compiler. JUCE is pulled in automatically via
CMake `FetchContent`, so no manual SDK install is needed.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### Linux build dependencies

On Debian/Ubuntu the JUCE GUI/audio modules need:

```bash
sudo apt-get install libasound2-dev libjack-jackd2-dev \
    libcurl4-openssl-dev libfreetype-dev libfontconfig1-dev \
    libx11-dev libxcomposite-dev libxcursor-dev libxext-dev \
    libxinerama-dev libxrandr-dev libxrender-dev \
    libwebkit2gtk-4.1-dev libglu1-mesa-dev mesa-common-dev
```

## Output

After building, the artefacts are written under `build/AudioPolish_artefacts/`:

- `VST3/Audio Polish.vst3` — copy to your VST3 folder
  (`~/.vst3` on Linux, `%CommonProgramFiles%\VST3` on Windows,
  `~/Library/Audio/Plug-Ins/VST3` on macOS)
- `Standalone/Audio Polish` — runs without a host

## Windows installer

A Windows installer is produced with [Inno Setup](https://jrsoftware.org/isinfo.php).
It installs the VST3 into the shared `Common Files\VST3` folder and, optionally, the
standalone app.

### Via CI (recommended)

The [`Windows Installer`](.github/workflows/windows-installer.yml) GitHub Actions
workflow builds the plugin with MSVC and compiles the installer on every push and on
manual dispatch:

1. Open the **Actions** tab → **Windows Installer** → **Run workflow** (or just push).
2. Download the `AudioPolish-Windows-Installer` artifact from the finished run.

Pushing a `v*` tag (e.g. `v1.0.0`) additionally attaches the installer to a GitHub
Release.

### Building the installer locally (on Windows)

```powershell
# 1. Build the plugin (MSVC)
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel

# 2. Compile the installer (Inno Setup 6 must be installed)
& "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe" /DAppVersion=1.0.0 installer\AudioPolish.iss
```

The installer is written to `installer\Output\AudioPolish-<version>-Windows-x64.exe`.

## Layout

```
CMakeLists.txt          # build + JUCE fetch
source/
  ParameterIDs.h        # parameter ids, ranges, defaults (single source of truth)
  PolishChain.h         # the DSP signal chain
  LoudnessMeter.h       # BS.1770 K-weighted LUFS-M/S/I meter
  PluginProcessor.*     # AudioProcessor + APVTS wiring, user preset I/O
  PluginEditor.*        # GUI: look-and-feel, knobs, meters
installer/
  AudioPolish.iss       # Inno Setup installer script (Windows)
.github/workflows/
  windows-installer.yml # CI: build on Windows + package installer
```
