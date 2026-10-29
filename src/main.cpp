#include "config.hpp"
#include "dashboard.hpp"
#include "input.hpp"
#include "output.hpp"
#include "receiver.hpp"
#include "session.hpp"
#include <chrono>
#include <cmath>
#include <csignal>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
constexpr const char* kVersion = "std-t99-receiver 0.1.0";
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
// Table Annex 2-3-1: CH1-9 from 142.934375 MHz, CH10-18 from 146.934375 MHz, 6.25 kHz steps. CH18 = calling channel.
double channel_hz(int channel) {
    return channel <= 9 ? 142934375.0 + (channel - 1) * 6250.0 : 146934375.0 + (channel - 10) * 6250.0;
}
void help() {
    std::cout << kVersion << R"( - RTL-SDR to STD-T99 (digital low-power community radio) .burst + .jsonl

  std-t99 --channel 18 -o capture
  std-t99 --channel 18 -o capture --llr --record-iq capture.cu8
  std-t99 --input capture.cu8 --sample-rate 1200000 --center-freq 146734375 --channel 18 -o replay

Options:
  --config FILE        Load RF defaults (key=value); command-line values win
  --channel N          Channel 1..18 (default 18 = calling channel 146.984375 MHz)
  -o, --output BASE    Output basename (default output; writes BASE.burst and BASE.jsonl)
  --llr                Also write soft decisions BASE.llr (288 bytes per .burst record)
  --record-iq FILE     Also save the raw RTL-SDR samples (cu8, 2.4 MB/s at 1.2 Msps)
  --overwrite          Allow replacing existing output files
  --device N           RTL-SDR device index (default 0)
  --gain auto|DB       Tuner gain (default auto; manual uses nearest supported gain)
  --ppm N              RTL-SDR oscillator correction in ppm (default 0)
  --freq-error HZ      Residual signal offset; positive shifts DSP tuning upward
  --seconds N          Stop after N seconds of input samples
  --sample-rate HZ     Input rate (default 1200000)
  --pfb 64|48          SPEAR PFB size (default 64)
  --center-freq HZ     Tuner / IQ center frequency (live default: channel - 250 kHz)
  --input FILE         Replay IQ instead of RTL-SDR: .cu8 (8-bit unsigned) or .cf32
                       (float32); requires --sample-rate and --center-freq
  --input-format F     cu8 or cf32 when the extension does not tell
  --ui auto|screen|lines  Full-screen view (default on a console) or one line per frame
  --rtl-library PATH   Explicit path to rtlsdr.dll / librtlsdr.so
  --list-channels      Print the channel table and exit
  --list-devices       List RTL-SDR devices and exit
  --help, --version

BASE.burst holds the voice frames (4 x 72-bit TCH per 80 ms, std-t98-receiver layout).
BASE.jsonl holds every received frame: raw bits, CRCs and decoded messages
(caller/called device ID, group, emergency, latitude/longitude, GPS time, caller name).
No vocoder runs here. Ctrl+C stops reception and closes the files.
)";
}
double number(const std::string& text, const std::string& option) {
    std::size_t used = 0;
    double result;
    try { result = std::stod(text, &used); }
    catch (...) { throw std::invalid_argument("Invalid value for " + option + ": " + text); }
    if (used != text.size() || !std::isfinite(result)) throw std::invalid_argument("Invalid value for " + option);
    return result;
}
int integer(const std::string& text, const std::string& option, int low, int high) {
    const double n = number(text, option);
    if (n < low || n > high || std::floor(n) != n) throw std::invalid_argument(option + " is outside its integer range");
    return static_cast<int>(n);
}
std::filesystem::path utf8_path(const std::string& s) { return std::filesystem::path(reinterpret_cast<const char8_t*>(s.c_str())); }
}

int main(int argc, char** argv) {
    try {
#ifdef _WIN32
        // CRT narrow argv uses the active ANSI code page. Preserve Japanese paths.
        int wide_count = 0;
        auto wide = CommandLineToArgvW(GetCommandLineW(), &wide_count);
        if (!wide) throw std::runtime_error("Cannot read command line");
        std::vector<std::string> utf8;
        for (int i = 0; i < wide_count; ++i) {
            int n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
            std::string value(static_cast<std::size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, value.data(), n, nullptr, nullptr);
            value.pop_back();
            utf8.push_back(std::move(value));
        }
        LocalFree(wide);
        std::vector<char*> pointers;
        for (auto& s : utf8) pointers.push_back(s.data());
        argc = wide_count;
        argv = pointers.data();
#endif
        t99::RadioOptions radio;
        int channel = 18, pfb = 64;
        std::string input, input_format, output = "output", record, ui = "auto";
        double center = 0, error = 0, seconds = 0;
        bool llr = false, overwrite = false, list = false, list_channels = false;
        bool rate_given = false, center_given = false;
        // Load configuration first, even when --config follows CLI overrides.
        std::vector<std::string> cli, args;
        bool config_given = false;
        const std::set<std::string> flags{"--help", "-h", "--version", "--llr", "--overwrite", "--list-devices", "--list-channels"};
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help" || arg == "-h") { help(); return 0; }
            if (arg == "--version") { std::cout << kVersion << '\n'; return 0; }
            if (arg == "--config") {
                if (config_given) throw std::invalid_argument("Specify --config only once");
                if (++i >= argc) throw std::invalid_argument("Missing value for --config");
                args = t99::read_rf_config(utf8_path(argv[i]));
                config_given = true;
            } else {
                cli.push_back(arg);
                if (!flags.contains(arg) && i + 1 < argc) cli.emplace_back(argv[++i]);
            }
        }
        args.insert(args.end(), cli.begin(), cli.end());
        for (std::size_t i = 0; i < args.size(); ++i) {
            const std::string arg = args[i];
            auto value = [&]() -> std::string { if (++i >= args.size()) throw std::invalid_argument("Missing value for " + arg); return args[i]; };
            if (arg == "--channel") channel = integer(value(), arg, 1, 18);
            else if (arg == "--device") radio.device = static_cast<unsigned>(integer(value(), arg, 0, 255));
            else if (arg == "--pfb") { pfb = integer(value(), arg, 48, 64); if (pfb != 48 && pfb != 64) throw std::invalid_argument("--pfb must be 64 or 48"); }
            else if (arg == "--output" || arg == "-o") output = value();
            else if (arg == "--input") input = value();
            else if (arg == "--input-format") { input_format = value(); if (input_format != "cu8" && input_format != "cf32") throw std::invalid_argument("--input-format must be cu8 or cf32"); }
            else if (arg == "--record-iq") record = value();
            else if (arg == "--ui") { ui = value(); if (ui != "auto" && ui != "screen" && ui != "lines") throw std::invalid_argument("--ui must be auto, screen or lines"); }
            else if (arg == "--rtl-library") radio.library = value();
            else if (arg == "--sample-rate") { radio.rate = static_cast<uint32_t>(integer(value(), arg, 300000, 10000000)); rate_given = true; }
            else if (arg == "--center-freq") { center = number(value(), arg); center_given = true; }
            else if (arg == "--freq-error") error = number(value(), arg);
            else if (arg == "--ppm") radio.ppm = integer(value(), arg, -1000, 1000);
            else if (arg == "--seconds") { seconds = number(value(), arg); if (seconds <= 0 || seconds > 86400) throw std::invalid_argument("--seconds must be in (0, 86400]"); }
            else if (arg == "--gain") { auto gain = value(); radio.automatic_gain = gain == "auto"; if (!radio.automatic_gain) { radio.gain_db = number(gain, arg); if (radio.gain_db < 0 || radio.gain_db > 60) throw std::invalid_argument("--gain must be auto or 0..60 dB"); } }
            else if (arg == "--llr") llr = true;
            else if (arg == "--overwrite") overwrite = true;
            else if (arg == "--list-devices") list = true;
            else if (arg == "--list-channels") list_channels = true;
            else throw std::invalid_argument("Unknown option: " + arg + " (see --help)");
        }
        if (list_channels) {
            for (int c = 1; c <= 18; ++c) std::cout << "CH" << std::setw(2) << c << "  " << std::fixed << std::setprecision(6) << channel_hz(c) / 1e6 << " MHz" << (c == 18 ? "  calling channel" : "") << '\n';
            return 0;
        }
        if (list) { t99::list_rtl(radio.library); return 0; }
        if (output.empty()) throw std::invalid_argument("--output must not be empty");
        bool u8 = false;
        if (!input.empty()) {
            if (!rate_given || !center_given) throw std::invalid_argument("IQ replay requires --sample-rate and --center-freq");
            if (radio.ppm != 0) throw std::invalid_argument("--ppm applies to hardware only; use --freq-error for recordings");
            if (!record.empty()) throw std::invalid_argument("--record-iq records the RTL-SDR; it cannot be combined with --input");
            const auto extension = utf8_path(input).extension().string();
            if (input_format.empty()) input_format = extension == ".cu8" ? "cu8" : extension == ".cf32" ? "cf32" : "";
            if (input_format.empty()) throw std::invalid_argument("Cannot tell the IQ format from the extension; add --input-format cu8|cf32");
            u8 = input_format == "cu8";
        }
        const double frequency = channel_hz(channel);
        if (!center_given) center = frequency - 250000;
        if (center < 1 || center > 2000000000 || std::floor(center) != center) throw std::invalid_argument("--center-freq must be an integer Hz value in 1..2000000000");
        radio.frequency = static_cast<uint32_t>(center);
        t99::ReceiverConfig cfg;
        cfg.in_rate = radio.rate;
        cfg.pfb_channels = pfb;
        if (radio.rate % static_cast<uint32_t>(pfb * 6250) != 0)
            throw std::invalid_argument("Rate must be a multiple of 400000 Hz for --pfb 64, or 300000 Hz for --pfb 48");
        cfg.freq_err_hz = frequency - center + error;
        if (std::abs(cfg.freq_err_hz) + 6250 >= radio.rate / 2.0) throw std::invalid_argument("Selected channel is outside the input IQ bandwidth");
        std::signal(SIGINT, stop);
        std::signal(SIGTERM, stop);
        auto source = input.empty() ? t99::open_rtl(radio) : t99::open_iq_file(utf8_path(input), u8);
        t99::Output writer(utf8_path(output), llr, overwrite);
        std::unique_ptr<t99::IqRecorder> recorder;
        if (!record.empty()) recorder = std::make_unique<t99::IqRecorder>(utf8_path(record), overwrite);
        t99::Receiver receiver(cfg);
        t99::Session session;
        std::cerr << "CH " << channel << " (" << std::fixed << std::setprecision(6) << frequency / 1e6 << " MHz), center "
                  << std::setprecision(6) << center / 1e6 << " MHz, " << radio.rate << " samples/s\n";
        std::cerr << "RF correction: " << radio.ppm << " ppm, residual " << std::setprecision(1) << error << " Hz\n";
        std::cerr << "SPEAR frontend: single selected channel, polyphase " << pfb << ", band rate " << pfb * 6250 << " samples/s\n";
        std::string outputs = output + ".burst + .jsonl" + (llr ? " + .llr" : "") + (record.empty() ? "" : " + IQ " + record);
        std::cerr << "Writing " << outputs << ". Ctrl+C to stop.\n";
        t99::Dashboard dashboard({channel, frequency, radio.rate, outputs, ui});
        auto last_report = std::chrono::steady_clock::now();
        bool drew_frame = false;
        receiver.set_frame_handler([&](const t99::Frame& frame) {
            const auto decoded = session.update(frame);
            const int64_t burst = writer.write_voice(frame);
            const double t = static_cast<double>(frame.input_sample_index) / radio.rate;
            writer.write_line(session.json(frame, decoded, t, burst));
            dashboard.on_frame(frame, decoded, session.state(), receiver.metrics(), writer.bursts());
            drew_frame = true;
            last_report = std::chrono::steady_clock::now();
        });
        dashboard.refresh(receiver.metrics(), writer.bursts(), 0);
        std::vector<spear::cf32> block(8192);
        uint64_t index = 0;
        const uint64_t limit = seconds > 0 ? static_cast<uint64_t>(seconds * radio.rate) : std::numeric_limits<uint64_t>::max();
        bool last_locked = false;
        while (!stopping && index < limit) {
            auto n = source->read(block, [] { return stopping != 0; });
            if (!n) break;
            n = static_cast<std::size_t>(std::min<uint64_t>(n, limit - index));
            if (recorder) recorder->write(std::span(block).first(n));
            drew_frame = false;
            receiver.process(std::span(block).first(n), index);
            index += n;
            const auto& m = receiver.metrics();
            if ((!drew_frame && m.locked != last_locked) ||
                std::chrono::steady_clock::now() - last_report >= dashboard.refresh_interval()) {
                dashboard.refresh(m, writer.bursts(), index);
                last_report = std::chrono::steady_clock::now();
            }
            last_locked = m.locked;
        }
        dashboard.close();
        const auto& m = receiver.metrics();
        std::cerr << "Saved " << writer.lines() << " frames (" << m.crc_frames << " with a valid CRC), " << writer.bursts()
                  << " voice bursts, " << index << " IQ samples.";
        if (m.have_offset) std::cerr << " Carrier offset estimate " << std::showpos << std::setprecision(0) << m.freq_offset_hz << std::noshowpos << " Hz.";
        std::cerr << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
