#pragma once
#include "receiver.hpp"
#include <array>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

namespace t99 {
// FF (48 + 9-byte payload) x 4: the record layout of std-t98-receiver, here 4 x 72-bit TCH frames
std::array<uint8_t, 41> encode_burst(std::span<const uint8_t> payload36);

class Output {
    std::ofstream burst_, llr_, jsonl_;
    uint64_t bursts_ = 0, lines_ = 0;
public:
    Output(const std::filesystem::path& base, bool llr, bool overwrite);
    // Writes the frame's voice to .burst/.llr when both halves are TCH; returns the record index or -1
    int64_t write_voice(const Frame& frame);
    void write_line(const std::string& json);
    uint64_t bursts() const { return bursts_; }
    uint64_t lines() const { return lines_; }
};

// Raw RTL-SDR samples (unsigned 8-bit I/Q, cu8) for later --input replay
class IqRecorder {
    std::ofstream stream_;
    std::vector<unsigned char> bytes_;
public:
    IqRecorder(const std::filesystem::path& path, bool overwrite);
    void write(std::span<const spear::cf32> samples);
};
}
