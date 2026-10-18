# LocalASR

Press a shortcut in any app, speak, watch the text appear live, click ✓ — the text is pasted into the input you
were typing in. Everything runs on the Mac: Qwen3-ASR or Parakeet on the GPU through Metal, no Python, no network.

```
LocalASR.app (Swift, menu bar)                      asrd (C++, one process)
  hotkey ─► floating panel (never takes focus)        Silero VAD ─► utterances
  AVAudioEngine ─► 16 kHz PCM ──── WebSocket ────►    speech.cpp runtime / ggml, Metal
  live text ◄──────────── partial / final ◄──────     re-reads the utterance under way every 0.3 s
  ✓ ─► pasteboard + ⌘V into the previous app
```

## Install

Download `LocalASR-macos-arm64.zip` from the latest [GitHub Release](https://github.com/dot5enko/LocalASR/releases), unzip it and move `LocalASR.app` to
/Applications. It needs macOS 14 or later on Apple silicon. If the release is not notarized, macOS refuses the first
launch: open System Settings › Privacy & Security and click **Open Anyway** (or run
`xattr -dr com.apple.quarantine /Applications/LocalASR.app`).

## Building

Needs Xcode or its command line tools (`xcode-select --install`, Swift 5.10 or later) and CMake 3.20 or later
(`brew install cmake`). speech.cpp and its ggml come in as git submodules, so clone with `--recursive` (or run
`git submodule update --init --recursive` in a clone made without it).

```bash
git clone --recursive https://github.com/dot5enko/LocalASR && cd LocalASR
make run      # builds asrd and LocalASR.app into dist/ and starts it
make install  # copies dist/LocalASR.app into /Applications
```

| Target | Does |
|---|---|
| `make server` | builds `build/asrd` and `build/asrd-feed` (CMake, Release) |
| `make app` | builds the Swift app and assembles and signs `dist/LocalASR.app` |
| `make models [MODEL=ID[:VARIANT]]` | downloads a model from a terminal (Qwen3-ASR 0.6B by default) |
| `make serve`, `make feed WAV=FILE.wav` | runs asrd in the foreground and streams a 16 kHz mono recording through it |
| `make clean` | removes `build/`, `dist/` and the Swift build |

The first model load compiles Metal shaders (~20 s, once); after that asrd is ready in about half a second.

## Usage

On first use macOS asks for the **Microphone**. To paste automatically, allow LocalASR under
**Privacy & Security › Accessibility** (menu bar icon › Allow Pasting); without it the text is copied and you press ⌘V.

| Key | Action |
|---|---|
| shortcut (menu › Shortcut…, any key with ⌘, ⌥ or ⌃) | start dictating; press again to insert |
| ✓ | insert |
| ↺ (Reset) | clear the text and keep listening |
| Esc or ✕ | cancel |

## Models (menu › Model…)

We selected few models for your convenience, other models might be implemente later by leveraging interface in backend.

| Model | Size | License | Notes |
|---|---|---|---|
| Parakeet TDT 0.6B v3 | 0.67 GB | CC-BY-4.0 | faster |
| Qwen3-ASR 0.6B | 0.84 GB | Apache-2.0 | a bit better for EN |

## Runtimes (menu › Runtime & Hardware…)

A runtime is what runs a model: speech.cpp (ggml, Metal) today. asrd sees runtimes only through the interfaces in
`server/core/runtime.h`: a `Runtime` lists its devices and load settings, reads a model's files (`inspect`) and loads a
`Recognizer` or a `VoiceDetector`. A model variant runs on any registered runtime that takes its format and
architecture, and a load names one or takes the first. Another runtime is a class in `server/runtimes/`, registered
in `runtimes/builtin.cpp`; the pipeline, the library and the app need no change. Runtime & Hardware shows the Mac,
each runtime with its devices and their free memory, and the settings the runtime and the loaded model declare
(device, threads, decode on CPU…), drawn from those declarations, and reloads the model with them.

## Layout

- `server/` — `asrd`: cpp-httplib server (`main.cpp`, routes in `api.cpp`).
  - `core/` — the runtime interfaces, the registry, `ModelManager` (loads and switches models), host hardware.
  - `library/` — catalog, the installed models (`library.cpp`), downloads (`downloads.cpp`, `transport_apple.mm`).
  - `pipeline/` — the dictation (`dictation.cpp`: VAD utterances, live re-reading) and language sets.
  - `runtimes/speechcpp/` — speech.cpp behind the interfaces.
  `asrd-feed` streams a recording to it in real time for testing (`make serve`, then `make feed WAV=FILE.wav`).
- `app/` — the Swift package; `app/scripts/bundle.sh` assembles `dist/LocalASR.app` with `asrd` inside and signs it
  (with an Apple Development identity when one is in the keychain, so permissions survive rebuilds).
- `third_party/speech.cpp` — submodule pinned to v0.8.8 (MIT): Qwen3-ASR, Parakeet and Silero VAD on ggml.

## Protocol (`ws://127.0.0.1:8765/v1/dictate?language=en,uk`)

`GET /health` reports `status` `ok`, `loading`, `no_model` or `failed` and the model's `id`, `runtime` and `device`;
`GET /v1/system` the Mac and the runtimes with their devices and settings; `GET /v1/models` the library, each variant
with whether it is `installed`, the runtimes that run it and its download. `POST /v1/models/load`
`{"model","variant","runtime","settings"}` switches models; `POST /v1/downloads` `{"model"}`, `GET /v1/downloads`,
`POST /v1/downloads/ID:VARIANT/pause|resume` and `DELETE` run downloads; and
`DELETE /v1/models/ID/VARIANT` removes a model. The full list heads `server/main.cpp`.

`language` is `auto`, one tag, or a comma-separated set. Client sends binary frames of 16-bit little-endian mono PCM at 16 kHz, `{"type":"reset"}` to drop the text so far, then `{"type":"stop"}` (or `cancel`).
Server sends `ready`, then `{"type":"partial","committed":"…","live":"…","epoch":0}` (display `committed + live`;
`epoch` counts resets, so a client drops partials older than its last reset), then
`{"type":"final","text":"…","language":"en","audio_s":8.6,"finalize_ms":113}` and closes. Requests that carry an
`Origin` header (web pages) are refused.

## Measured (M3, Qwen3-ASR-0.6B Q8_0)

- 13.9 s clip transcribed in 0.89 s (RTFx 15.6).
- Live text trails speech by ~0.3–0.5 s; the final transcript arrives 0.1–0.5 s after stop.

## License

MIT, see [LICENSE](LICENSE). speech.cpp and ggml are MIT too. The models are downloaded under their own licenses
(see Models).
