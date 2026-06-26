# Audio Polish

A "make it sound finished" VST3 / Standalone audio plugin built with [JUCE 8](https://juce.com).
One **Polish** macro takes a track or mix from flat to finished, with full per-stage
control underneath when you want it.

## Signal chain

```
Input gain
  -> Tone        (low shelf + high shelf, with a Tilt fold-in)
  -> Drive       (asymmetric tanh saturation, 4x oversampled, DC-blocked)
  -> Glue        (program-dependent compression)
  -> Width       (mid/side widening, low end kept mono as width increases)
  -> Output gain
  -> Ceiling     (brick-wall limiter)
  -> Mix         (latency-compensated dry/wet)
```

The **Polish** macro pushes Drive, Glue, top-end "air" and Width together so a single
knob does the heavy lifting; the individual controls trim from there.

### DSP quality notes

- **Anti-aliased saturation** — the waveshaper runs inside a 4× oversampled,
  linear-phase block, so the harmonics it generates don't fold back as aliasing.
- **Even-harmonic warmth** — the saturator is mildly asymmetric (tube-like) rather
  than a pure odd-harmonic `tanh`; a DC blocker removes the resulting offset.
- **Frequency-conscious width** — as Width goes past 100 %, the low end of the side
  signal is progressively collapsed to mono so the bass stays centred and phase-safe.
- **Latency reporting** — the oversampler's latency is reported to the host, and both
  the dry/wet Mix path and Bypass are delay-compensated to stay sample-aligned.

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
| Bypass    | —                | Latency-compensated bypass                              |

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

## Layout

```
CMakeLists.txt          # build + JUCE fetch
source/
  ParameterIDs.h        # parameter ids, ranges, defaults (single source of truth)
  PolishChain.h         # the DSP signal chain
  PluginProcessor.*     # AudioProcessor + APVTS wiring
  PluginEditor.*        # GUI: look-and-feel, knobs, meter
```
