// External tools: exiftool (metadata), ffmpeg (video/ugoira conversion), ImageMagick (image conversion).
#pragma once

#include <string>
#include <vector>

#include "core/util.hpp"

namespace bg::external {

struct ProcessResult {
    bool started = false;
    bool timedOut = false;
    int exitCode = -1;
    std::string out, err;
    bool ok() const { return started && !timedOut && exitCode == 0; }
};

// Runs argv[0] with arguments, waits at most timeoutMs
ProcessResult run(const StringList& argv, int timeoutMs = 30000);
bool available(const std::string& program);

enum class Sidecar { No, OnError, Both, Only };
bool exiftoolSetMetadata(const std::string& file, const std::vector<std::pair<std::string, std::string>>& metadata, bool clear,
                         bool keepColorProfile, Sidecar sidecar, bool sidecarNoExtension);

std::string ffmpegVideoCodec(const std::string& file);
std::string ffmpegRemux(const std::string& file, const std::string& ext, bool overwrite, bool deleteOriginal, int timeoutMs);
std::string ffmpegConvert(const std::string& file, const std::string& ext, bool overwrite, bool deleteOriginal, int timeoutMs);
// Converts a Pixiv ugoira zip into an animation using the frame delays (ms)
std::string ffmpegConvertUgoira(const std::string& file, const std::vector<std::pair<std::string, int>>& frames, const std::string& ext,
                                bool overwrite, bool deleteOriginal, int timeoutMs);
std::string imageMagickConvert(const std::string& file, const std::string& ext, bool overwrite, bool deleteOriginal, int timeoutMs);

// Minimal ZIP extraction (stored and deflated entries)
bool unzip(const std::string& zip, const std::string& destDir, StringList* files = nullptr);

}  // namespace bg::external
