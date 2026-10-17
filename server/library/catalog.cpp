#include "catalog.h"

namespace asrd {

namespace {

const std::vector<std::string> kQwenLanguages = {"ar", "cs", "da", "de", "el", "en", "es", "fa", "fi", "fil",
                                                 "fr", "hi", "hu", "id", "it", "ja", "ko", "mk", "ms", "nl",
                                                 "pl", "pt", "ro", "ru", "sv", "th", "tr", "vi", "yue", "zh"};

const std::vector<std::string> kParakeetLanguages = {"bg", "cs", "da", "de", "el", "en", "es", "et", "fi",
                                                     "fr", "hr", "hu", "it", "lt", "lv", "mt", "nl", "pl",
                                                     "pt", "ro", "ru", "sk", "sl", "sv", "uk"};

// The files speech.cpp v0.8.8 catalogs.
const std::vector<ModelSpec> kCatalog = {
    {"parakeet-tdt-0.6b-v3", Task::recognition, "parakeet-tdt-0.6b-v3", "nvidia/parakeet-tdt-0.6b-v3", "0.6B",
     "CC-BY-4.0", "https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3", kParakeetLanguages, false,
     {{"q8_0", "gguf", "fastconformer", "Q8_0",
       {{"sakasegawa/parakeet-tdt-0.6b-v3-GGUF", "b7ed82dbfba6e63da0ef9cf5104f5fc67da44414",
         "parakeet-tdt-0.6B-v3-Q8_0.gguf", 668'107'680,
         "7e61f3f5bb8db536ae649a541eb23349833d3d62565aff1da5bcd7c4f163639a"}}}},
     {"silero-vad"}},
    {"qwen3-asr-0.6b", Task::recognition, "Qwen3-ASR-0.6B", "Qwen/Qwen3-ASR-0.6B", "0.6B",
     "Apache-2.0", "https://huggingface.co/Qwen/Qwen3-ASR-0.6B", kQwenLanguages, true,
     {{"q8_0", "gguf", "qwen3-asr", "Q8_0",
       {{"sakasegawa/Qwen3-ASR-0.6B-GGUF", "ee78f8709547f39bea3b924a16c72403826cac92", "Qwen3-ASR-0.6B-Q8_0.gguf",
         841'502'336, "416e10c15b4a3d9002bd337d18fc450233fdf68502b6e10d1379d2789838afd0"}}}},
     {"silero-vad"}},
    {"silero-vad", Task::detection, "silero-vad", "snakers4/silero-vad", "309K", "MIT",
     "https://github.com/snakers4/silero-vad", {}, false,
     {{"f32", "gguf", "silero-vad", "F32",
       {{"sakasegawa/silero-vad-GGUF", "7fcb7aa2a70c312f8579fc987b3e1cdecf9321ca", "silero-vad-309K-v6.2-F32.gguf",
         1'238'624, "2fc312514e5fc4fa4ffd02b25c76a4853fa4130e8fb9777d01502183b01238cb"}}}},
     {}},
};

}  // namespace

std::string FileSpec::url() const {
    return "https://huggingface.co/" + repository + "/resolve/" + revision + "/" + path;
}

std::string FileSpec::name() const {
    return path.substr(path.find_last_of('/') + 1);
}

int64_t Variant::bytes() const {
    int64_t total = 0;
    for (const FileSpec & file : files) total += file.bytes;
    return total;
}

const Variant * ModelSpec::variant(const std::string & id) const {
    for (const Variant & v : variants)
        if (v.id == id) return &v;
    return nullptr;
}

const std::vector<ModelSpec> & catalog() { return kCatalog; }

const ModelSpec * catalog_find(const std::string & id) {
    for (const ModelSpec & model : kCatalog)
        if (model.id == id) return &model;
    return nullptr;
}

const ModelSpec & catalog_default_recognizer() { return *catalog_find("qwen3-asr-0.6b"); }

}  // namespace asrd
