.PHONY: all models server app install run serve feed clean

all: app

# Downloads models into ~/Library/Application Support/LocalASR/models (asrd pull checks each file's pinned hash and
# brings Silero VAD along): Qwen3-ASR 0.6B by default, `make models MODEL=parakeet-tdt-0.6b-v3` for Parakeet.
MODEL ?= qwen3-asr-0.6b
models: server
	build/asrd pull $(MODEL)

server:
	cmake -B build -DCMAKE_BUILD_TYPE=Release
	cmake --build build --target asrd asrd-feed -j

app: server
	app/scripts/bundle.sh

install: app
	-pkill -x LocalASR
	rm -rf /Applications/LocalASR.app
	cp -R dist/LocalASR.app /Applications/

run: app
	@# The running app stops its asrd as it quits; open would otherwise reach the instance still quitting.
	-pkill -x LocalASR
	@while pgrep -x LocalASR >/dev/null || pgrep -x asrd >/dev/null; do sleep 0.2; done
	open dist/LocalASR.app

# asrd in the foreground; the app uses it instead of starting its own.
serve: server
	build/asrd -v

# Streams a recording (16 kHz mono 16-bit WAV) through a running asrd in real time: make feed WAV=speech.wav.
feed: server
	@test -n "$(WAV)" || { echo "usage: make feed WAV=FILE.wav" >&2; exit 1; }
	build/asrd-feed "$(WAV)"

clean:
	rm -rf build dist app/.build
