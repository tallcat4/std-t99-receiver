#include "input.hpp"
#include "iq_queue.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace t99 {
namespace {
class FileInput final : public Input {
    std::ifstream stream_;
    bool u8_;
    std::vector<unsigned char> bytes_;
public:
    // cf32: little-endian float32 I/Q. cu8: unsigned 8-bit I/Q as delivered by RTL-SDR (--record-iq).
    FileInput(const std::filesystem::path& path, bool u8) : stream_(path, std::ios::binary), u8_(u8) {
        static_assert(std::endian::native == std::endian::little);
        static_assert(sizeof(spear::cf32) == 8);
        if (!stream_) throw std::runtime_error("Cannot open IQ file: " + path.string());
        if (std::filesystem::file_size(path) % (u8 ? 2 : 8)) throw std::runtime_error(u8 ? "cu8 IQ file must contain complete 2-byte I/Q samples" : "cf32 IQ file must contain complete 8-byte I/Q samples");
    }
    std::size_t read(std::span<spear::cf32> samples, bool (*stop_requested)()) override {
        if (stop_requested && stop_requested()) return 0;
        if (u8_) {
            bytes_.resize(samples.size() * 2);
            stream_.read(reinterpret_cast<char*>(bytes_.data()), static_cast<std::streamsize>(bytes_.size()));
            if (stream_.bad()) throw std::runtime_error("Error reading IQ file");
            const auto n = static_cast<std::size_t>(stream_.gcount()) / 2;
            for (std::size_t i = 0; i < n; ++i) samples[i] = {(bytes_[2*i] - 127.5f) / 128.f, (bytes_[2*i+1] - 127.5f) / 128.f};
            return n;
        }
        stream_.read(reinterpret_cast<char*>(samples.data()), static_cast<std::streamsize>(samples.size_bytes()));
        if (stream_.bad()) throw std::runtime_error("Error reading IQ file");
        return static_cast<std::size_t>(stream_.gcount()) / 8;
    }
};

// Runtime loading keeps replay and --help independent of any SDR installation.
// Signatures are the stable librtlsdr C API; no GNU Radio/UHD dependency.
class Library {
#ifdef _WIN32
    HMODULE handle_{};
#else
    void* handle_{};
#endif
public:
    explicit Library(const std::string& requested) {
        std::vector<std::string> names;
        if (!requested.empty()) names.push_back(requested);
#ifdef _WIN32
        else names = {"rtlsdr.dll", "librtlsdr.dll"};
        for (const auto& name : names) { handle_ = LoadLibraryW(std::filesystem::path(reinterpret_cast<const char8_t*>(name.c_str())).c_str()); if (handle_) break; }
#else
        else names = {"librtlsdr.so.0", "librtlsdr.so", "librtlsdr.dylib"};
        for (const auto& name : names) { handle_ = dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL); if (handle_) break; }
#endif
        if (!handle_) throw std::runtime_error("Cannot load librtlsdr. Put the matching 64-bit rtlsdr.dll and libusb-1.0.dll beside the exe, or use --rtl-library PATH. See README.");
    }
    ~Library() {
#ifdef _WIN32
        if (handle_) FreeLibrary(handle_);
#else
        if (handle_) dlclose(handle_);
#endif
    }
    template<class T> T get(const char* name) {
#ifdef _WIN32
        auto address = GetProcAddress(handle_, name);
#else
        auto address = dlsym(handle_, name);
#endif
        if (!address) throw std::runtime_error(std::string("Missing librtlsdr function: ") + name);
        return reinterpret_cast<T>(address);
    }
};

struct Device;
class RtlInput final : public Input {
    Library library_;
    Device* device_ = nullptr;
    int (*close_)(Device*);
    using Callback = void (*)(unsigned char*, uint32_t, void*);
    int (*read_)(Device*, Callback, void*, uint32_t, uint32_t);
    int (*cancel_)(Device*);
    IqQueue queue_;
    std::thread worker_;
    std::atomic<bool> stopping_{false}, finished_{false};
    std::vector<unsigned char> bytes_;
    static void receive(unsigned char* data, uint32_t size, void* context) {
        auto& self = *static_cast<RtlInput*>(context);
        if (self.stopping_ || !self.queue_.push({data, size})) self.cancel_(self.device_);
    }
public:
    explicit RtlInput(const std::string& library) : library_(library) {
        close_ = library_.get<decltype(close_)>("rtlsdr_close");
        read_ = library_.get<decltype(read_)>("rtlsdr_read_async");
        cancel_ = library_.get<decltype(cancel_)>("rtlsdr_cancel_async");
    }
    ~RtlInput() override {
        stopping_ = true;
        if (worker_.joinable()) {
            // Cancellation before read_async starts is ignored by librtlsdr.
            // Retry until the worker exits, including on an immediate Ctrl+C.
            while (!finished_) {
                cancel_(device_);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            worker_.join();
        }
        if (device_) close_(device_);
    }
    void configure(const RadioOptions& o) {
        auto count = library_.get<uint32_t(*)()>("rtlsdr_get_device_count");
        if (o.device >= count()) throw std::runtime_error("RTL-SDR device not found. Connect it, install WinUSB (Interface 0), then run --list-devices.");
        auto open = library_.get<int(*)(Device**, uint32_t)>("rtlsdr_open");
        check(open(&device_, o.device), "open device");
        auto rate = library_.get<int(*)(Device*, uint32_t)>("rtlsdr_set_sample_rate");
        auto freq = library_.get<int(*)(Device*, uint32_t)>("rtlsdr_set_center_freq");
        auto ppm = library_.get<int(*)(Device*, int)>("rtlsdr_set_freq_correction");
        auto gain_mode = library_.get<int(*)(Device*, int)>("rtlsdr_set_tuner_gain_mode");
        auto agc = library_.get<int(*)(Device*, int)>("rtlsdr_set_agc_mode");
        auto reset = library_.get<int(*)(Device*)>("rtlsdr_reset_buffer");
        auto get_rate = library_.get<uint32_t(*)(Device*)>("rtlsdr_get_sample_rate");
        check(rate(device_, o.rate), "set sample rate");
        if (get_rate(device_) != o.rate) throw std::runtime_error("RTL-SDR did not accept the requested sample rate");
        if (o.ppm != 0) check(ppm(device_, o.ppm), "set frequency correction");
        check(freq(device_, o.frequency), "tune frequency");
        check(gain_mode(device_, o.automatic_gain ? 0 : 1), "set tuner gain mode");
        check(agc(device_, o.automatic_gain ? 1 : 0), "set RTL AGC");
        if (!o.automatic_gain) {
            auto gains = library_.get<int(*)(Device*, int*)>("rtlsdr_get_tuner_gains");
            auto set_gain = library_.get<int(*)(Device*, int)>("rtlsdr_set_tuner_gain");
            const int n = gains(device_, nullptr);
            if (n <= 0 || n > 1024) throw std::runtime_error("Cannot query tuner gains");
            std::vector<int> values(static_cast<std::size_t>(n));
            if (gains(device_, values.data()) != n) throw std::runtime_error("Tuner gain list changed");
            const int desired = static_cast<int>(std::lround(o.gain_db * 10));
            const int gain = *std::min_element(values.begin(), values.end(), [desired](int a, int b) { return std::abs(a-desired) < std::abs(b-desired); });
            check(set_gain(device_, gain), "set gain");
            std::cerr << "Tuner gain: " << gain / 10.0 << " dB\n";
        }
        check(reset(device_), "reset buffer");
    }
    std::size_t read(std::span<spear::cf32> samples, bool (*stop_requested)()) override {
        if (samples.empty() || (stop_requested && stop_requested())) return 0;
        if (!worker_.joinable()) worker_ = std::thread([this] {
            // Keep USB transfers queued while the caller demodulates or writes.
            // 16 KiB transfers arrive every ~6.8 ms at 1.2 Msps.
            const int result = read_(device_, receive, this, 16, 16384);
            queue_.finish(result < 0 || !stopping_);
            finished_ = true;
        });
        bytes_.resize(samples.size() * 2);
        const auto received = queue_.pop(bytes_, stop_requested);
        for (std::size_t i = 0; i < received / 2; ++i)
            samples[i] = {(bytes_[2*i] - 127.5f) / 128.f, (bytes_[2*i+1] - 127.5f) / 128.f};
        return received / 2;
    }
    static void check(int result, const char* operation) {
        if (result < 0) throw std::runtime_error(std::string("librtlsdr: cannot ") + operation + " (" + std::to_string(result) + ")");
    }
};
}

std::unique_ptr<Input> open_iq_file(const std::filesystem::path& path, bool u8) { return std::make_unique<FileInput>(path, u8); }
std::unique_ptr<Input> open_rtl(const RadioOptions& options) {
    auto input = std::make_unique<RtlInput>(options.library);
    input->configure(options);
    return input;
}
void list_rtl(const std::string& path) {
    Library library(path);
    auto count = library.get<uint32_t(*)()>("rtlsdr_get_device_count");
    auto name = library.get<const char*(*)(uint32_t)>("rtlsdr_get_device_name");
    const auto n = count();
    for (uint32_t i = 0; i < n; ++i) std::cout << i << ": " << name(i) << '\n';
    if (!n) std::cout << "No RTL-SDR devices found.\n";
}
}
