// The models asrd offers, declared once. A model comes in variants, each a format and quantization of its weights
// with the files that make it up; a variant runs on any registered runtime that takes its format and architecture.
// Each file is pinned to a revision and a checksum.
#pragma once

#include "runtime.h"

#include <cstdint>
#include <string>
#include <vector>

namespace asrd {

struct FileSpec {
    std::string repository;   // on Hugging Face
    std::string revision;
    std::string path;         // in the repository
    int64_t bytes = 0;
    std::string sha256;

    std::string url() const;
    std::string name() const; // the file name, which it keeps on disk
};

struct Variant {
    std::string id;           // "q8_0"
    std::string format;       // "gguf"
    std::string architecture; // "qwen3-asr"
    std::string quantization; // "Q8_0"
    std::vector<FileSpec> files;

    int64_t bytes() const;
};

struct ModelSpec {
    std::string id;           // what asrd pull, the API and the app name it by
    Task task = Task::recognition;
    std::string name;         // the original model's name, as its card heads it
    std::string upstream;     // "nvidia/parakeet-tdt-0.6b-v3": who publishes the original model
    std::string parameters;   // "0.6B"
    std::string license;
    std::string source;       // the original model's card, which the license refers to
    std::vector<std::string> languages;  // shown before the model is downloaded; the runtime's reading wins after
    bool language_steers = false;
    std::vector<Variant> variants;       // the first is the default
    std::vector<std::string> requires;   // models it needs beside it: the voice activity detector

    const Variant * variant(const std::string & id) const;
};

const std::vector<ModelSpec> & catalog();
const ModelSpec * catalog_find(const std::string & id);
const ModelSpec & catalog_default_recognizer();

}  // namespace asrd
