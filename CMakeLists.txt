cmake_minimum_required(VERSION 3.20)
# asrd runs wherever the app does (LSMinimumSystemVersion), not only on the macOS it was built on.
set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "")
project(local-asr LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# speech.cpp: Qwen3-ASR and Silero VAD on ggml (Metal on macOS). Only the targets asrd links get built.
add_subdirectory(third_party/speech.cpp EXCLUDE_FROM_ALL)

option(ASRD_WITH_SPEECHCPP "Build the speech.cpp runtime into asrd" ON)

# The runtime interfaces, the model library with its downloads, and the dictation pipeline: nothing here knows a
# runtime. Each runtime is a library of its own that runtimes/builtin.cpp registers.
add_library(asrd-core STATIC
    server/core/json.cpp
    server/core/runtime.cpp
    server/core/registry.cpp
    server/core/hardware.cpp
    server/core/model_manager.cpp
    server/library/catalog.cpp
    server/library/library.cpp
    server/library/downloads.cpp
    server/library/transport_apple.mm
    server/pipeline/dictation.cpp
    server/pipeline/languages.cpp
)
target_include_directories(asrd-core PUBLIC server server/core server/library server/pipeline)
target_compile_options(asrd-core PRIVATE -Wall -Wextra)
# The one Objective-C++ file (URLSession), compiled by the C++ compiler as such.
set_source_files_properties(server/library/transport_apple.mm PROPERTIES LANGUAGE CXX COMPILE_FLAGS "-x objective-c++ -fobjc-arc")
target_link_libraries(asrd-core PUBLIC "-framework Foundation")

add_executable(asrd
    server/main.cpp
    server/api.cpp
    server/runtimes/builtin.cpp
)
target_include_directories(asrd PRIVATE server/runtimes third_party/speech.cpp/vendor/cpp-httplib)
target_link_libraries(asrd PRIVATE asrd-core)
target_compile_options(asrd PRIVATE -Wall -Wextra)

if (ASRD_WITH_SPEECHCPP)
    add_library(asrd-runtime-speechcpp STATIC server/runtimes/speechcpp/speechcpp.cpp)
    target_include_directories(asrd-runtime-speechcpp PRIVATE $<TARGET_PROPERTY:asrd-core,INTERFACE_INCLUDE_DIRECTORIES>)
    target_link_libraries(asrd-runtime-speechcpp PRIVATE speech-static)
    target_compile_options(asrd-runtime-speechcpp PRIVATE -Wall -Wextra)
    target_link_libraries(asrd PRIVATE asrd-runtime-speechcpp)
    target_compile_definitions(asrd PRIVATE ASRD_WITH_SPEECHCPP=1)
endif()

# Streams a recording to asrd in real time, for checking it without the app.
add_executable(asrd-feed server/feed.cpp)
target_include_directories(asrd-feed PRIVATE third_party/speech.cpp/vendor/cpp-httplib)
