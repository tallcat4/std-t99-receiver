#include "output.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace t99 {
std::array<uint8_t, 41> encode_burst(std::span<const uint8_t> payload) {
    if (payload.size() != 36) throw std::invalid_argument("A traffic burst must have 4 x 9 bytes");
    std::array<uint8_t, 41> result{};
    result[0] = 0xff;
    for (std::size_t i = 0; i < 4; ++i) {
        result[1 + i*10] = 0x48;
        std::copy_n(payload.begin() + static_cast<long>(i*9), 9, result.begin() + static_cast<long>(2 + i*10));
    }
    return result;
}

namespace {
void open(std::ofstream& stream, const std::filesystem::path& path, bool overwrite) {
    if (!overwrite && std::filesystem::exists(path)) throw std::runtime_error("Output exists: " + path.string() + ". Choose another --output or use --overwrite.");
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    stream.open(path, std::ios::binary | std::ios::trunc);
    if (!stream) throw std::runtime_error("Cannot create output: " + path.string());
    stream.exceptions(std::ios::badbit | std::ios::failbit);
}
}

Output::Output(const std::filesystem::path& base, bool llr, bool overwrite) {
    auto suffix = [&](const char* ext) { auto path = base; path += ext; return path; };
    std::vector<std::pair<std::ofstream*, std::filesystem::path>> files{{&burst_, suffix(".burst")}, {&jsonl_, suffix(".jsonl")}};
    if (llr) files.push_back({&llr_, suffix(".llr")});
    for (const auto& [stream, path] : files)
        if (!overwrite && std::filesystem::exists(path)) throw std::runtime_error("Output exists: " + path.string() + ". Choose another --output or use --overwrite.");
    for (const auto& [stream, path] : files) open(*stream, path, true);
}

int64_t Output::write_voice(const Frame& frame) {
    if (!frame.voice()) return -1;
    std::array<uint8_t, 36> payload{};
    std::copy(frame.halves[0].payload.begin(), frame.halves[0].payload.end(), payload.begin());
    std::copy(frame.halves[1].payload.begin(), frame.halves[1].payload.end(), payload.begin() + 18);
    const auto bytes = encode_burst(payload);
    burst_.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    burst_.flush();
    if (llr_.is_open()) {
        for (const auto& h : frame.halves) llr_.write(reinterpret_cast<const char*>(h.llr.data()), h.llr.size());
        llr_.flush();
    }
    return static_cast<int64_t>(bursts_++);
}

void Output::write_line(const std::string& json) {
    jsonl_ << json << '\n';
    jsonl_.flush();
    ++lines_;
}

IqRecorder::IqRecorder(const std::filesystem::path& path, bool overwrite) { open(stream_, path, overwrite); }
void IqRecorder::write(std::span<const spear::cf32> samples) {
    // Exact inverse of the RTL conversion (b - 127.5) / 128
    bytes_.resize(samples.size() * 2);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        bytes_[2*i] = static_cast<unsigned char>(std::clamp(std::lround(samples[i].real() * 128.f + 127.5f), 0L, 255L));
        bytes_[2*i+1] = static_cast<unsigned char>(std::clamp(std::lround(samples[i].imag() * 128.f + 127.5f), 0L, 255L));
    }
    stream_.write(reinterpret_cast<const char*>(bytes_.data()), static_cast<std::streamsize>(bytes_.size()));
}
}
