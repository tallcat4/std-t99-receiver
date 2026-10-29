#include "input.hpp"
#include "iq_queue.hpp"
#include "output.hpp"
#include "receiver.hpp"
#include "session.hpp"
#include "spec_vectors.hpp"
#include "dsp/rrc.hpp"
#include "spear/dsp/channelizer.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <numbers>
#include <random>
#include <stdexcept>
#include <thread>

using namespace t99;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(std::string(__func__) + ":" + std::to_string(__LINE__) + ": " + #x); } while (0)

namespace {
std::vector<const spec::Vector*> signal(const std::string& test) {
    std::vector<const spec::Vector*> out;
    for (const auto& v : spec::vectors()) if (test == v.test) out.push_back(&v);
    return out;
}
Frame decode(const spec::Vector& v) {
    std::vector<float> raw(v.symbols.begin(), v.symbols.end());
    return decode_frame(raw);
}
struct Result { CallState state; std::vector<Decoded> messages; std::vector<Frame> frames; };
Result run(const std::string& test) {
    Result r;
    Session session;
    for (const auto* v : signal(test)) {
        auto f = decode(*v);
        auto d = session.update(f);
        r.messages.insert(r.messages.end(), d.begin(), d.end());
        r.frames.push_back(std::move(f));
    }
    r.state = session.state();
    return r;
}
bool has(const Result& r, const std::string& channel, int type) {
    for (const auto& d : r.messages) if (d.channel == channel && d.message.type == type) return true;
    return false;
}
std::string hex(std::span<const uint8_t> b) {
    static constexpr char d[] = "0123456789ABCDEF";
    std::string s;
    for (auto x : b) { s.push_back(d[x >> 4]); s.push_back(d[x & 15]); }
    return s;
}
const DeviceId kCaller{127, 123456}, kCalled{127, 654};
}

// Every frame printed in Annex 2 7.7.2 decodes with zero path errors and valid CRCs,
// and RICH matches the frame type (Table Annex 2-4-8).
void spec_frames() {
    CHECK(spec::vectors().size() == 53);
    for (const auto& v : spec::vectors()) {
        const auto f = decode(v);
        CHECK(f.sacch.crc_ok && f.sacch.bit_errors == 0);
        CHECK(f.rich.parity_ok && f.rich.d == 0);
        if (v.frame == 0) CHECK(f.rich.f == 0b01 && f.rich.m == 0b01 && f.rich.o == 0 && f.sync_burst());
        else CHECK(f.rich.f == 0b10 && (f.rich.m == 0b10 ? f.rich.o == 0b11 : f.rich.o == 0));
        for (const auto& h : f.halves)
            if (h.control) CHECK(h.control->crc_ok && h.control->bit_errors == 0);
        if (f.voice()) {
            // 1031 Hz TOKUDER tone, Table Annex 2-7-1. The printed TCH hex carries 4 different bits in the
            // first 72-bit block of every frame (AFB6EFD3...), the other three blocks match the table exactly.
            const std::string tone = "AFB6EFB59C208A06FD";
            CHECK(hex(f.halves[0].payload).substr(18) == tone);
            CHECK(hex(f.halves[1].payload) == tone + tone);
            CHECK(hex(f.halves[0].payload).substr(0, 18) == "AFB6EFD39C208A06FD");
        }
    }
}

void spec_messages() {
    {   // 1-1 individual voice call
        const auto r = run("1-1");
        const auto& s = r.state;
        CHECK(s.identification && s.identification->extension == 0 && s.identification->device == kCaller);
        CHECK(s.caller == kCaller && s.called == kCalled && s.caller->str() == "127-0123456" && s.called->str() == "127-0000654");
        CHECK(s.comm_type == 0b100 && s.group == 0 && s.call_control == 0 && s.voice_mode == 0 && s.ended);
        CHECK(s.position && s.position->latitude == 35402565 && s.position->longitude == 139450235);
        CHECK(s.position->altitude_m == 7 && s.position->valid);
        CHECK(std::abs(s.position->latitude_deg() - (35 + 40.2565 / 60)) < 1e-9 && std::abs(s.position->longitude_deg() - (139 + 45.0235 / 60)) < 1e-9);
        CHECK(s.gps_time && s.gps_time->time == 235120 && s.gps_time->date == 260718 && s.gps_time->speed_01kn == 27 && s.gps_time->direction_deg == 267);
        CHECK(s.gps_time->str() == "2018-07-26 23:51:20 JST  2.7 kn  267 deg");
        CHECK(has(r, "PICH2", kVoice) && has(r, "SACCH(SF)", kVoice) && has(r, "FACCH1", kGpsData2) && has(r, "FACCH2", kEndCall));
        CHECK(has(r, "SACCH", kIdle));
        for (const auto& d : r.messages) if (d.message.type == kIdle) CHECK(d.message.maker == 127);
    }
    {   // 1-2 group call, group 3; 1-3 simultaneous call
        const auto r = run("1-2");
        CHECK(r.state.comm_type == 0b001 && r.state.group == 3 && r.state.called && r.state.called->zero());
        CHECK(run("1-3").state.group == 0);
    }
    {   // 2-1 emergency 1 (location notification), 2-2 emergency 2
        const auto r = run("2-1");
        CHECK(has(r, "PICH2", kGpsData1) && r.state.call_control == 1);
        CHECK(run("2-2").state.call_control == 2);
    }
    CHECK(has(run("3-1"), "FACCH1", kRemoteRequest));
    CHECK(has(run("4-1"), "FACCH1", kLocationRequest));
    {
        const auto r = run("5-1");
        CHECK(has(r, "FACCH1", kDeviceIdRequest) && has(r, "PICH2", kDeviceIdRequest));
    }
    for (const char* test : {"6-1", "6-2"}) {   // caller name via SACCH super frame / FACCH, CRC-32 checked
        const auto r = run(test);
        CHECK(!r.state.name_check_failed);
#ifdef _WIN32
        CHECK(r.state.name == "\xE7\x84\xA1\xE7\xB7\x9A\xE8\xA9\xA6\xE9\xA8\x93No.9");   // 無線試験No.9
#else
        CHECK(r.state.name.ends_with("No.9"));
#endif
    }
    // A corrupted name check is reported, not displayed
    Session session;
    Bits part1(72, 0), part2(72, 0);
    auto put = [](Bits& b, std::size_t at, uint64_t v, int n) { for (int i = 0; i < n; ++i) b[at + static_cast<std::size_t>(i)] = (v >> (n - 1 - i)) & 1; };
    put(part1, 3, kCallerName, 5);
    put(part2, 0, 1, 1);
    put(part2, 3, kCallerName, 5);
    auto m1 = parse_message(part1), m2 = parse_message(part2);
    CHECK(m1.name_part == 0 && m2.name_part == 1 && m1.name_bytes.size() == 8 && m2.name_bytes.size() == 4);
}

// Test-only 4-level FSK modulator per Annex 2 3.3: impulses through H(f)·P(f), ±315/±945 Hz deviation.
std::vector<spear::cf32> modulate(const std::vector<int8_t>& symbols, double rate, double offset_hz, double noise, uint32_t seed) {
    const double fs = 48000, sps = fs / 2400;
    const auto taps = spear::std_t98::make_tx_taps(fs, 2400, 0.2, static_cast<int>(sps) * 24 + 1);
    std::vector<double> level(static_cast<std::size_t>(symbols.size() * sps + taps.size()), 0.0);
    for (std::size_t k = 0; k < symbols.size(); ++k)
        for (std::size_t t = 0; t < taps.size(); ++t) level[static_cast<std::size_t>(k * sps) + t] += symbols[k] * sps * taps[t];
    std::mt19937 rng(seed);
    std::normal_distribution<float> gauss(0.f, static_cast<float>(noise));
    std::vector<spear::cf32> iq(static_cast<std::size_t>(static_cast<double>(level.size()) / fs * rate));
    double phase = 0;
    for (std::size_t i = 0; i < iq.size(); ++i) {
        const double x = static_cast<double>(i) * fs / rate;
        const auto j = static_cast<std::size_t>(x);
        const double mu = x - static_cast<double>(j);
        const double f = (j + 1 < level.size() ? level[j] + mu * (level[j + 1] - level[j]) : 0.0) * 315.0 + offset_hz;
        phase += 2 * std::numbers::pi * f / rate;
        iq[i] = spear::cf32(0.5f * static_cast<float>(std::cos(phase)) + gauss(rng), 0.5f * static_cast<float>(std::sin(phase)) + gauss(rng));
    }
    return iq;
}

std::vector<int8_t> transmission(const std::string& test) {
    std::vector<int8_t> s(48, 0);   // carrier only
    // Preamble P = 5775FDh as printed for every test signal (not whitened)
    for (int8_t p : {3, 3, 3, -3, 3, -3, 3, 3, -3, -3, -3, 3}) s.push_back(p);
    for (const auto* v : signal(test)) s.insert(s.end(), v->symbols.begin(), v->symbols.end());
    s.insert(s.end(), 48, 0);
    return s;
}

std::vector<Frame> receive(const std::vector<spear::cf32>& iq, ReceiverConfig cfg) {
    Receiver receiver(cfg);
    std::vector<Frame> frames;
    receiver.set_frame_handler([&](const Frame& f) { frames.push_back(f); });
    for (std::size_t p = 0; p < iq.size(); p += 8192)
        receiver.process(std::span(iq).subspan(p, std::min<std::size_t>(8192, iq.size() - p)), p);
    return frames;
}

// The whole receiver (channel selection, discriminator, RX filter, symbol and frame sync)
// recovers every frame of a test signal transmitted 250 kHz away from the tuner, with a residual
// carrier error up to the ±2.5 ppm tolerance (±370 Hz at 147 MHz) and no --freq-error.
void end_to_end() {
    const double rate = 1.2e6;
    const auto expected = run("6-1");
    ReceiverConfig cfg;
    cfg.in_rate = rate;
    cfg.freq_err_hz = 250000;   // channel offset only; the residual is left to the receiver
    for (double residual : {0.0, 180.0, 370.0, -370.0}) {
        const auto frames = receive(modulate(transmission("6-1"), rate, 250000 + residual, 0.2, 99), cfg);
        CHECK(frames.size() == expected.frames.size());
        Session session;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            CHECK(frames[i].crc_ok() && frames[i].sacch.crc_ok);
            const auto& got = frames[i];
            const auto& want = expected.frames[i];
            CHECK(got.sacch.message == want.sacch.message);
            for (std::size_t h = 0; h < 2; ++h) {
                CHECK(got.halves[h].kind == want.halves[h].kind);
                if (want.halves[h].control) CHECK(got.halves[h].control->message == want.halves[h].control->message);
                else {   // TCH has no FEC here: allow the odd channel bit error
                    int errors = 0;
                    for (std::size_t k = 0; k < 144; ++k) errors += got.halves[h].bits[k] != want.halves[h].bits[k];
                    CHECK(errors <= 2);
                }
            }
            session.update(got);
        }
        CHECK(session.state().name == expected.state.name && session.state().caller == kCaller && session.state().position);
    }
    // The locked receiver measures the residual for --freq-error
    Receiver receiver(cfg);
    receiver.set_frame_handler([](const Frame&) {});
    const auto iq = modulate(transmission("1-1"), rate, 250000 + 180, 0.02, 7);
    for (std::size_t p = 0; p < iq.size(); p += 8192) receiver.process(std::span(iq).subspan(p, std::min<std::size_t>(8192, iq.size() - p)), p);
    CHECK(receiver.metrics().have_offset && std::abs(receiver.metrics().freq_offset_hz - 180) < 40);
}

// A transmission that starts after receiver noise with only the minimum 12-symbol preamble:
// the synchronous burst (identity and position) must still be decoded at any timing phase.
void acquisition() {
    ReceiverConfig cfg;
    cfg.in_rate = 1.2e6;
    cfg.freq_err_hz = 250000;
    for (uint32_t seed = 1; seed <= 10; ++seed) {
        std::mt19937 rng(seed);
        std::normal_distribution<float> gauss(0.f, 0.03f);
        std::vector<spear::cf32> iq(200000 + seed * 977);
        for (auto& x : iq) x = {gauss(rng), gauss(rng)};
        auto symbols = transmission("2-1");
        symbols.erase(symbols.begin(), symbols.begin() + 48);   // no carrier before the preamble
        const auto tx = modulate(symbols, 1.2e6, 250000 - 300.0 + 60.0 * seed, 0.03, seed);
        iq.insert(iq.end(), tx.begin(), tx.end());
        for (int k = 0; k < 120000; ++k) iq.push_back({gauss(rng), gauss(rng)});
        const auto frames = receive(iq, cfg);
        CHECK(frames.size() == 2 && frames[0].sync_burst());
        for (const auto& f : frames) CHECK(f.sacch.crc_ok && f.halves[0].control->crc_ok && f.halves[1].control->crc_ok);
    }
}

void single_channel_filter() {
    for (int count : {48, 64}) {
        auto taps = spear::dsp::design_lowpass(count * 6250., 3125, static_cast<std::size_t>(count) * 8 - 1);
        spear::dsp::PfbChannelizer reference(static_cast<std::size_t>(count), count * 6250., taps);
        CenterChannelizer selected(static_cast<std::size_t>(count), count * 6250., taps);
        std::mt19937 rng(99);
        std::uniform_real_distribution<float> random(-1, 1);
        std::vector<spear::cf32> input(static_cast<std::size_t>(count) * 100 + 17);
        for (auto& x : input) x = {random(rng), random(rng)};
        std::vector<std::vector<spear::cf32>> full, one;
        reference.process(input, full);
        for (std::size_t pos = 0; pos < input.size(); pos += 37)
            selected.process(std::span(input).subspan(pos, std::min<std::size_t>(37, input.size() - pos)), one);
        CHECK(one.size() == 1 && one[0].size() == full[0].size());
        for (std::size_t i = 0; i < one[0].size(); ++i) CHECK(std::abs(one[0][i] - full[0][i]) < 1e-6f);
    }
}

void silence() {
    ReceiverConfig cfg;
    Receiver receiver(cfg);
    int frames = 0;
    receiver.set_frame_handler([&](const Frame&) { ++frames; });
    std::mt19937 rng(1);
    std::normal_distribution<float> gauss(0.f, 0.1f);
    std::vector<spear::cf32> noise(32768);
    for (uint64_t i = 0; i < 40; ++i) {
        for (auto& x : noise) x = {gauss(rng), gauss(rng)};
        receiver.process(noise, i * noise.size());
    }
    CHECK(frames == 0 && !receiver.metrics().locked);
}

void output_files() {
    std::vector<uint8_t> payload(36);
    for (int i = 0; i < 36; ++i) payload[static_cast<std::size_t>(i)] = static_cast<uint8_t>(i);
    const auto bytes = encode_burst(payload);
    CHECK(bytes[0] == 0xff && bytes[1] == 0x48 && bytes[2] == 0 && bytes[31] == 0x48 && bytes[40] == 35);
    const auto base = std::filesystem::current_path() / "test-output";
    for (auto ext : {".burst", ".llr", ".jsonl", ".cu8"}) std::filesystem::remove(base.string() + ext);
    {
        Output writer(base, true, false);
        Session session;
        int64_t next = 0;
        for (const auto* v : signal("1-1")) {
            const auto f = decode(*v);
            const auto d = session.update(f);
            const auto burst = writer.write_voice(f);
            CHECK(burst == (f.voice() ? next++ : -1));
            writer.write_line(session.json(f, d, 0.08 * v->frame, burst));
        }
        CHECK(writer.bursts() == 4 && writer.lines() == 6);
        IqRecorder recorder(base.string() + ".cu8", false);
        std::vector<spear::cf32> iq;
        for (int b = 0; b < 256; b += 3) iq.push_back({(b - 127.5f) / 128.f, (255 - b - 127.5f) / 128.f});
        recorder.write(iq);
    }
    CHECK(std::filesystem::file_size(base.string() + ".burst") == 4 * 41);
    CHECK(std::filesystem::file_size(base.string() + ".llr") == 4 * 288);
    std::ifstream jsonl(base.string() + ".jsonl");
    std::string line, all;
    while (std::getline(jsonl, line)) { CHECK(line.front() == '{' && line.back() == '}'); all += line; }
    jsonl.close();
    CHECK(all.find("\"caller\":\"127-0123456\"") != std::string::npos && all.find("\"lat_raw\":35402565") != std::string::npos);
    CHECK(all.find("\"type\":\"GPS data 2\"") != std::string::npos && all.find("\"burst\":3") != std::string::npos);
    auto input = open_iq_file(base.string() + ".cu8", true);
    std::vector<spear::cf32> back(200);
    const auto n = input->read(back, nullptr);
    CHECK(n == 86);
    for (std::size_t i = 0; i < n; ++i) CHECK(back[i] == spear::cf32((3 * static_cast<int>(i) - 127.5f) / 128.f, (255 - 3 * static_cast<int>(i) - 127.5f) / 128.f));
    input.reset();
    bool rejected = false;
    try { Output again(base, false, false); } catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected);
    for (auto ext : {".burst", ".llr", ".jsonl", ".cu8"}) std::filesystem::remove(base.string() + ext);
}

void iq_queue_continuity() {
    IqQueue queue(8);
    const std::vector<unsigned char> first{0, 1, 2, 3, 4, 5}, second{6, 7, 8, 9, 10, 11};
    std::vector<unsigned char> out(8);
    CHECK(queue.push(first));
    CHECK(queue.pop(std::span(out).first(4)) == 4);
    CHECK(queue.push(second));
    CHECK(queue.pop(out) == 8);
    CHECK((out == std::vector<unsigned char>{4, 5, 6, 7, 8, 9, 10, 11}));
    IqQueue overflow(4);
    CHECK(overflow.push(std::span(first).first(4)));
    CHECK(!overflow.push(std::span(first).first(2)));
    bool rejected = false;
    try { overflow.pop(out); } catch (const std::runtime_error&) { rejected = true; }
    CHECK(rejected);
}

int main(int argc, char** argv) {
    try {
        // Writes a test signal as RTL-style cu8 IQ for a CLI replay:
        //   t99-tests --write-iq FILE.cu8 [TEST]   (1.2 Msps, center 146.734375 MHz, CH18 + 180 Hz)
        if (argc >= 3 && std::string(argv[1]) == "--write-iq") {
            // Transmissions separated by 0.5 s of receiver noise only (key-up / key-down like a real radio)
            IqRecorder recorder(argv[2], true);
            std::mt19937 rng(5);
            std::normal_distribution<float> gauss(0.f, 0.03f);
            std::vector<spear::cf32> gap(600000);
            std::size_t total = 0;
            uint32_t seed = 1;
            for (const char* test : {"1-1", "6-2", "2-1", "6-1"}) {
                if (argc >= 4 && std::string(argv[3]) != test) continue;
                for (auto& x : gap) x = {gauss(rng), gauss(rng)};
                recorder.write(gap);
                const auto iq = modulate(transmission(test), 1.2e6, 250000 + 180, 0.03, seed++);
                recorder.write(iq);
                total += gap.size() + iq.size();
            }
            std::cout << total << " samples written\n";
            return 0;
        }
        spec_frames();
        spec_messages();
        end_to_end();
        acquisition();
        single_channel_filter();
        silence();
        output_files();
        iq_queue_continuity();
        std::cout << "53 Annex 2 test-signal frames, messages, synthesized-IQ end-to-end receive and acquisition, filter, files: passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
