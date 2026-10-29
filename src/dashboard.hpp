// Full-screen terminal view (ANSI / Windows VT, no curses dependency).
// Every decoded item shows where it came from (PICH1, PICH2, SACCH super frame, FACCH),
// how many times it was received with a valid CRC and how long ago, so that it is easy
// to see which information is actually being decoded from the air.
#pragma once
#include "session.hpp"
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace t99 {
struct DashboardInfo {
    int channel = 0;
    double frequency_hz = 0;
    uint32_t sample_rate = 0;
    std::string outputs;
    std::string ui = "auto";   // auto: full screen on a console, lines otherwise; screen; lines
};

class Dashboard {
public:
    explicit Dashboard(DashboardInfo info);
    ~Dashboard();
    Dashboard(const Dashboard&) = delete;
    Dashboard& operator=(const Dashboard&) = delete;

    void on_frame(const Frame& frame, const std::vector<Decoded>& decoded, const CallState& call,
                  const ChannelMetrics& metrics, uint64_t bursts);
    void refresh(const ChannelMetrics& metrics, uint64_t bursts, uint64_t input_samples);
    std::chrono::milliseconds refresh_interval() const {
        return terminal_ ? std::chrono::milliseconds(100) : std::chrono::milliseconds(1000);
    }
    void close();

private:
    struct Item {                 // one decoded information element
        std::string value, source;
        uint64_t count = 0;
        double time = -1;         // input time of the last update [s]
        bool alert = false;
    };
    struct Counter { uint64_t ok = 0, total = 0; };

    DashboardInfo info_;
    bool terminal_ = false, screen_ = false, closed_ = false;
    bool have_frame_ = false, have_log_status_ = false, last_log_synced_ = false;
    Frame last_frame_;
    double now_ = 0;              // input time [s]
    std::map<std::string, Item> items_;
    std::map<std::string, Counter> counters_;
    std::map<std::string, uint64_t> message_counts_;
    std::deque<std::string> log_;
    std::size_t previous_width_ = 0;
#ifdef _WIN32
    HANDLE console_handle_ = INVALID_HANDLE_VALUE;
    DWORD original_mode_ = 0;
    UINT original_output_cp_ = 0;
#endif
    void set(const std::string& key, std::string value, const std::string& source, bool alert = false);
    void paint(const ChannelMetrics& metrics, uint64_t bursts, bool frame_event);
};

// One-line text for logs and non-terminal output
std::string describe(const Frame& frame, const std::vector<Decoded>& decoded);
}
