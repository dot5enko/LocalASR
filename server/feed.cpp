// asrd-feed: streams a recording to asrd as a microphone would, in real time, and prints the events with their times.
//
//   asrd-feed FILE.wav|FILE.pcm [ws://127.0.0.1:8765/v1/dictate] [--fast] [--reset-at SECONDS]
//
// FILE is 16-bit mono PCM at 16 kHz, raw or in a WAVE file whose 44-byte header is skipped.
#include <httplib.h>

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

int main(int argc, char ** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: asrd-feed FILE.wav|FILE.pcm [URL] [--fast] [--reset-at SECONDS]\n");
        return 2;
    }
    const std::string path = argv[1];
    std::string url = "ws://127.0.0.1:8765/v1/dictate";
    bool fast = false;
    double reset_at = -1;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--fast") fast = true;
        else if (arg == "--reset-at" && i + 1 < argc) reset_at = std::atof(argv[++i]);
        else url = arg;
    }
    std::ifstream in(path, std::ios::binary);
    std::string pcm((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (path.size() > 4 && path.substr(path.size() - 4) == ".wav") pcm.erase(0, 44);
    if (pcm.empty()) {
        std::fprintf(stderr, "asrd-feed: cannot read %s\n", path.c_str());
        return 1;
    }

    httplib::ws::WebSocketClient ws(url);
    if (!ws.connect()) {
        std::fprintf(stderr, "asrd-feed: cannot connect to %s\n", url.c_str());
        return 1;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const auto elapsed = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    std::thread reader([&] {
        std::string message;
        while (ws.read(message) != httplib::ws::Fail) std::printf("%6.2fs  %s\n", elapsed(), message.c_str());
    });

    // 100 ms frames, as the app sends them.
    const size_t frame = 3200;
    for (size_t at = 0; at < pcm.size(); at += frame) {
        if (reset_at >= 0 && static_cast<double>(at) / 32000 >= reset_at) {
            std::printf("%6.2fs  -> reset\n", elapsed());
            ws.send(std::string(R"({"type":"reset"})"));
            reset_at = -1;
        }
        ws.send(pcm.data() + at, std::min(frame, pcm.size() - at));
        if (!fast) std::this_thread::sleep_until(t0 + std::chrono::milliseconds(100 * (at / frame + 1)));
    }
    std::printf("%6.2fs  -> stop\n", elapsed());
    ws.send(std::string(R"({"type":"stop"})"));
    reader.join();
    return 0;
}
