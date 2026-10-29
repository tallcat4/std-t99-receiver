#pragma once
#include "spear/core/types.hpp"
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace t99 {
struct RadioOptions {
    unsigned device = 0;
    uint32_t rate = 1200000;
    uint32_t frequency = 142684375;
    int ppm = 0;
    bool automatic_gain = true;
    double gain_db = 30;
    std::string library;
};
class Input {
public:
    virtual ~Input() = default;
    virtual std::size_t read(std::span<spear::cf32> samples, bool (*stop_requested)() = nullptr) = 0;
};
std::unique_ptr<Input> open_iq_file(const std::filesystem::path& path, bool u8);
std::unique_ptr<Input> open_rtl(const RadioOptions& options);
void list_rtl(const std::string& library);
}
