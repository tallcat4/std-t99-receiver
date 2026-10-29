#include "dashboard.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>
#ifndef _WIN32
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace t99 {
namespace {
// Terminal columns of a UTF-8 string (East Asian wide characters count 2)
std::size_t columns(const std::string& s, std::size_t* bytes_for = nullptr, std::size_t limit = 0) {
    std::size_t width = 0;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        const std::size_t n = c < 0x80 ? 1 : c >> 5 == 6 ? 2 : c >> 4 == 14 ? 3 : 4;
        uint32_t cp = n == 1 ? c : c & (0x3f >> (n - 1));
        for (std::size_t k = 1; k < n && i + k < s.size(); ++k) cp = cp << 6 | (static_cast<unsigned char>(s[i + k]) & 0x3f);
        const std::size_t w = (cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0xa4cf) || (cp >= 0xac00 && cp <= 0xd7a3) ||
                              (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xfe30 && cp <= 0xfe4f) || (cp >= 0xff00 && cp <= 0xff60) ||
                              (cp >= 0xffe0 && cp <= 0xffe6) ? 2 : 1;
        if (bytes_for && width + w > limit) { *bytes_for = i; return width; }
        width += w;
        i += n;
    }
    if (bytes_for) *bytes_for = s.size();
    return width;
}
std::string fit(std::string value, std::size_t width) {
    if (columns(value) > width) {
        std::size_t cut = 0;
        columns(value, &cut, width > 3 ? width - 3 : width);
        value.resize(cut);
        if (width > 3) value += "...";
    }
    value.append(width - std::min(width, columns(value)), ' ');
    return value;
}
std::string signed_hz(double hz) {
    std::ostringstream s;
    s << std::showpos << std::fixed << std::setprecision(0) << hz << " Hz";
    return s.str();
}
std::string age(double seconds) {
    if (seconds < 0) return "";
    char b[32];
    if (seconds < 60) std::snprintf(b, sizeof b, "%.1fs", seconds);
    else std::snprintf(b, sizeof b, "%.0fm%02.0fs", std::floor(seconds / 60), std::fmod(seconds, 60));
    return b;
}
const char* half_name(HalfKind k) { return k == HalfKind::tch ? "TCH" : k == HalfKind::pich ? "PICH" : "FACCH"; }
std::string message_summary(const Decoded& d) {
    if (d.channel == "PICH1") return "PICH1 Identification";
    return d.channel + " " + message_name(d.message.type);
}
std::string details(const Decoded& d) {
    std::ostringstream o;
    const auto& m = d.message;
    if (m.caller) o << ' ' << m.caller->str();
    if (m.called) o << " -> " << m.called->str();
    if (m.comm_type >= 0) o << ' ' << comm_type_name(m.comm_type);
    if (m.position) o << ' ' << m.position->str();
    if (m.gps_time) o << ' ' << m.gps_time->str();
    if (m.call_control > 0) o << ' ' << call_control_name(m.call_control);
    if (m.maker >= 0) o << " maker " << m.maker;
    if (m.name_part >= 0) o << " part " << m.name_part + 1 << "/2";
    return o.str();
}
// Display order of the decoded items: key, label, section
struct Row { const char* key; const char* label; int section; };
constexpr Row kRows[] = {
    {"caller", "Caller ID", 0}, {"ext", "ID ext code", 0}, {"called", "Called ID", 0}, {"comm", "Call type", 0},
    {"group", "Group no.", 0}, {"control", "Call control", 0}, {"voice", "Voice mode", 0}, {"message", "Last message", 0},
    {"name", "Caller name", 0}, {"maker", "Maker (idle)", 0},
    {"latlon", "Lat / Lon", 1}, {"decimal", "Decimal deg", 1}, {"altitude", "Alt / fix", 1},
    {"gpstime", "GPS time", 1}, {"motion", "Speed / dir", 1},
};
}

std::string describe(const Frame& f, const std::vector<Decoded>& decoded) {
    std::ostringstream o;
    o << (f.sync_burst() ? "SB" : "SC") << " F" << f.rich.f << " M" << f.rich.m << " O" << f.rich.o
      << (f.sync_ok ? "" : " flywheel") << " SACCH " << (f.sacch.crc_ok ? "OK" : "NG");
    for (const auto& h : f.halves) {
        o << ' ' << half_name(h.kind);
        if (h.control) o << (h.control->crc_ok ? ":OK" : ":NG");
    }
    for (const auto& d : decoded) o << " | " << message_summary(d) << details(d);
    return o.str();
}

Dashboard::Dashboard(DashboardInfo info) : info_(std::move(info)) {
#ifdef _WIN32
    original_output_cp_ = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);   // caller names, degree signs and box drawing
    console_handle_ = GetStdHandle(STD_ERROR_HANDLE);
    if (console_handle_ != INVALID_HANDLE_VALUE && console_handle_ &&
        GetConsoleMode(console_handle_, &original_mode_)) {
        terminal_ = true;
        screen_ = SetConsoleMode(console_handle_, original_mode_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    }
#else
    terminal_ = isatty(STDERR_FILENO);
    screen_ = terminal_;
#endif
    if (info_.ui == "lines") screen_ = false;
    else if (info_.ui == "screen") screen_ = true;   // also when redirected (120 x 50)
    if (screen_) std::cerr << "\x1b[?1049h\x1b[?25l\x1b[2J" << std::flush;
}

Dashboard::~Dashboard() { close(); }

void Dashboard::close() {
    if (closed_) return;
    if (screen_) std::cerr << "\x1b[?25h\x1b[?1049l" << std::flush;
    else if (terminal_ && previous_width_) std::cerr << '\n';
#ifdef _WIN32
    if (terminal_ && console_handle_ != INVALID_HANDLE_VALUE) SetConsoleMode(console_handle_, original_mode_);
    if (original_output_cp_) SetConsoleOutputCP(original_output_cp_);
#endif
    closed_ = true;
}

void Dashboard::set(const std::string& key, std::string value, const std::string& source, bool alert) {
    auto& item = items_[key];
    item.value = std::move(value);
    item.source = source;
    item.alert = alert;
    item.time = now_;
    ++item.count;
}

void Dashboard::on_frame(const Frame& frame, const std::vector<Decoded>& decoded, const CallState& call,
                         const ChannelMetrics& metrics, uint64_t bursts) {
    now_ = static_cast<double>(frame.input_sample_index) / info_.sample_rate;
    last_frame_ = frame;
    have_frame_ = true;
    auto count = [&](const std::string& key, bool ok) { auto& c = counters_[key]; ++c.total; if (ok) ++c.ok; };
    count("SACCH", frame.sacch.crc_ok);
    for (const auto& h : frame.halves) {
        if (h.kind == HalfKind::tch) count("TCH", true);
        else count(h.kind == HalfKind::pich ? "PICH" : "FACCH", h.control && h.control->crc_ok);
    }
    if (frame.sacch.crc_ok) set("group", frame.sacch.group == 0 ? "0 (all stations)" : std::to_string(frame.sacch.group), "SACCH W");

    char t[32];
    std::snprintf(t, sizeof t, "%8.2f ", now_);
    if (frame.sync_burst() && frame.halves[0].control && frame.halves[0].control->crc_ok) {
        log_.push_front(std::string(t) + "---- synchronous burst: new transmission ----");
        counters_["Transmissions"].ok++;
    }
    for (const auto& d : decoded) {
        const auto& m = d.message;
        ++message_counts_[d.channel == "PICH1" ? std::string("Identification") : message_name(m.type)];
        if (d.channel == "SACCH(SF)") counters_["Superframes"].ok++;
        log_.push_front(std::string(t) + message_summary(d) + details(d));
        if (d.channel == "PICH1") {
            const auto id = parse_identification(m.bits);
            set("caller", id.device.str(), "PICH1");
            set("ext", std::to_string(id.extension), "PICH1");
            set("called", id.called.zero() ? "000-0000000 (group / all)" : id.called.str(), "PICH1");
            continue;
        }
        if (m.type != kIdle && m.type >= 0) set("message", message_name(m.type), d.channel);
        if (m.caller && !m.caller->zero()) set("caller", m.caller->str(), d.channel);
        if (m.called) set("called", m.called->zero() ? "000-0000000 (group / all)" : m.called->str(), d.channel);
        if (m.comm_type == 0b001 || m.comm_type == 0b100) set("comm", comm_type_name(m.comm_type), d.channel);
        if (m.call_control >= 0) set("control", call_control_name(m.call_control), d.channel, m.call_control == 1 || m.call_control == 2);
        if (m.voice_mode >= 0) set("voice", voice_mode_name(m.voice_mode), d.channel);
        if (m.maker >= 0) set("maker", std::to_string(m.maker), d.channel);
        if (m.position) {
            const auto& p = *m.position;
            set("latlon", p.str(), d.channel, !p.valid);
            char b[64];
            std::snprintf(b, sizeof b, "%.6f, %.6f", p.latitude_deg(), p.longitude_deg());
            set("decimal", b, d.channel, !p.valid);
            set("altitude", std::to_string(p.altitude_m) + " m   fix " + (p.valid ? "A (valid)" : "V (invalid)"), d.channel, !p.valid);
        }
        if (m.gps_time) {
            const auto& g = *m.gps_time;
            char b[64];
            std::snprintf(b, sizeof b, "20%02u-%02u-%02u %02u:%02u:%02u JST", g.date % 100, g.date / 100 % 100, g.date / 10000,
                          g.time / 10000, g.time / 100 % 100, g.time % 100);
            if (g.date == 0) set("gpstime", "no fix (date 0)", d.channel, true);   // sent while GPS has no fix
            else set("gpstime", b, d.channel);
            std::snprintf(b, sizeof b, "%d.%d kn   %d deg", g.speed_01kn / 10, g.speed_01kn % 10, g.direction_deg);
            set("motion", b, d.channel);
        }
        if (m.type == kCallerName && m.name_part == 1) {
            if (call.name_check_failed) set("name", "(CRC-32 check NG)", d.channel, true);
            else if (!call.name.empty()) set("name", call.name + "   (CRC-32 OK)", d.channel);
        }
    }
    while (log_.size() > 200) log_.pop_back();
    if (!screen_) {
        if (terminal_ && previous_width_) { std::cerr << '\r' << std::string(previous_width_, ' ') << '\r'; previous_width_ = 0; }
        std::cerr << describe(frame, decoded) << '\n';
        return;
    }
    paint(metrics, bursts, true);
}

void Dashboard::refresh(const ChannelMetrics& metrics, uint64_t bursts, uint64_t input_samples) {
    now_ = std::max(now_, static_cast<double>(input_samples) / info_.sample_rate);
    paint(metrics, bursts, false);
}

void Dashboard::paint(const ChannelMetrics& m, uint64_t bursts, bool frame_event) {
    const bool synced = frame_event || m.locked;
    if (!screen_) {
        std::ostringstream s;
        s << "RX " << (synced ? "OPEN" : "WAIT") << "  Power " << std::fixed << std::setprecision(1) << m.power_db
          << " dB  Frames " << m.frames << "  CRC " << m.crc_frames << "  Voice " << bursts;
        if (m.have_offset) s << "  Offset " << signed_hz(m.freq_offset_hz);
        const auto value = s.str();
        if (terminal_) {
            std::cerr << '\r' << value;
            if (value.size() < previous_width_) std::cerr << std::string(previous_width_ - value.size(), ' ');
            std::cerr << std::flush;
            previous_width_ = value.size();
        } else if (!have_log_status_ || synced != last_log_synced_) std::cerr << value << '\n';
        have_log_status_ = true;
        last_log_synced_ = synced;
        return;
    }

    int width = terminal_ ? 80 : 120, height = terminal_ ? 24 : 50;
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO sb{};
    if (GetConsoleScreenBufferInfo(console_handle_, &sb)) {
        width = sb.srWindow.Right - sb.srWindow.Left + 1;
        height = sb.srWindow.Bottom - sb.srWindow.Top + 1;
    }
#else
    winsize size{};
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &size) == 0) {
        if (size.ws_col) width = size.ws_col;
        if (size.ws_row) height = size.ws_row;
    }
#endif
    constexpr auto dim = "\x1b[90m", normal = "\x1b[37m", cyan = "\x1b[1;36m", green = "\x1b[1;32m",
                   yellow = "\x1b[1;33m", red = "\x1b[1;31m", white = "\x1b[1;37m";
    struct Line { std::string text; const char* color; };
    std::vector<Line> lines;
    const std::size_t inner = static_cast<std::size_t>(std::max(20, std::min(width - 1, 140))) - 4;
    auto horizontal = [&](const std::string& title, const char* left, const char* right) {
        std::string t = title.empty() ? std::string() : "─ " + title + " ";
        std::string s = std::string(left) + t;
        const std::size_t used = columns(t);
        for (std::size_t i = used; i < inner + 2; ++i) s += "─";
        lines.push_back({s + right, cyan});
    };
    auto add = [&](const std::string& text, const char* color = nullptr) {
        lines.push_back({"│ " + fit(text, inner) + " │", color ? color : normal});
    };
    // label | value | source | received | age
    const std::size_t value_width = inner > 62 ? inner - 48 : 14;
    auto item_row = [&](const Row& r) {
        const auto it = items_.find(r.key);
        if (it == items_.end()) {
            add(fit(std::string("  ") + r.label, 16) + fit("--", value_width), dim);
            return;
        }
        const auto& i = it->second;
        const double a = now_ - i.time;
        std::ostringstream o;
        o << fit(std::string("✓ ") + r.label, 16) << fit(i.value, value_width) << ' ' << fit(i.source, 10)
          << ' ' << std::setw(6) << i.count << "x  " << fit(age(a), 8);
        add(o.str(), i.alert ? red : a > 10 ? yellow : green);
    };

    std::ostringstream title;
    title << "STD-T99 RECEIVER  CH" << std::setfill('0') << std::setw(2) << info_.channel << std::setfill(' ')
          << (info_.channel == 18 ? " (calling)" : "") << "  " << std::fixed << std::setprecision(6) << info_.frequency_hz / 1e6
          << " MHz  t=" << std::setprecision(1) << now_ << "s";
    horizontal(title.str(), "┌", "┐");
    std::ostringstream radio;
    radio << (synced ? "● LOCKED " : "○ WAITING") << "   Power " << std::showpos << std::fixed << std::setprecision(1) << m.power_db
          << " dB" << std::noshowpos << "   Carrier offset " << (m.have_offset ? signed_hz(m.freq_offset_hz) : std::string("--"))
          << "   Symbol period " << std::setprecision(2) << m.sps;
    add(radio.str(), synced ? green : yellow);
    std::ostringstream frames;
    frames << "Frames " << m.frames << "   any CRC OK " << m.crc_frames << "   flywheel " << m.flywheel_frames
           << "   locks " << m.locks << "   transmissions " << counters_["Transmissions"].ok << "   voice bursts " << bursts;
    add(frames.str(), m.frames && m.crc_frames < m.frames ? yellow : normal);
    std::ostringstream crc;
    auto ratio = [&](const char* key) { const auto& c = counters_[key]; return std::to_string(c.ok) + "/" + std::to_string(c.total); };
    crc << "CRC OK   SACCH " << ratio("SACCH") << "   PICH " << ratio("PICH") << "   FACCH " << ratio("FACCH")
        << "   SACCH superframes " << counters_["Superframes"].ok << "   TCH halves " << counters_["TCH"].total;
    add(crc.str());
    if (m.have_offset && std::abs(m.freq_offset_hz) > 300)
        add("Carrier is " + signed_hz(m.freq_offset_hz) + " off the channel (tracked automatically; --freq-error " + signed_hz(m.freq_offset_hz) + " would center it)", yellow);
    add("Output  " + info_.outputs, dim);

    for (int section = 0; section < 2; ++section) {
        horizontal(section == 0 ? "CALL / IDENTITY" : "POSITION (GPS data 1 / PICH2, GPS data 2)", "├", "┤");
        add(fit("  item", 16) + fit("value", value_width) + ' ' + fit("source", 10) + "  received  age", dim);
        for (const auto& r : kRows) if (r.section == section) item_row(r);
    }
    horizontal("MESSAGES RECEIVED", "├", "┤");
    {
        std::string s;
        for (const auto& [name, n] : message_counts_) s += (s.empty() ? "" : "   ") + name + " " + std::to_string(n);
        add(s.empty() ? "--" : s, s.empty() ? dim : normal);
    }
    horizontal("LAST FRAME", "├", "┤");
    if (have_frame_) {
        const auto& f = last_frame_;
        std::ostringstream r;
        r << (f.sync_burst() ? "Synchronous burst" : f.rich.f == 0b10 ? "Communication channel" : "Unknown (RICH F reserved)")
          << "   RICH F" << (f.rich.f >> 1) << (f.rich.f & 1) << " M" << (f.rich.m >> 1) << (f.rich.m & 1)
          << " O" << (f.rich.o >> 1) << (f.rich.o & 1) << (f.rich.parity_ok ? " parity OK" : " parity NG")
          << "   sync " << (f.sync_ok ? "OK" : "flywheel") << "  SSE " << std::fixed << std::setprecision(1) << f.sync_sse;
        add(r.str(), f.sync_ok ? normal : yellow);
        add(std::string(f.sacch.crc_ok ? "✓" : "✗") + " SACCH   W " + std::to_string(f.sacch.structure >> 1) + std::to_string(f.sacch.structure & 1) +
            " group " + std::to_string(f.sacch.group) + "  unit " + to_hex(f.sacch.unit) + "  path errors " + std::to_string(f.sacch.bit_errors),
            f.sacch.crc_ok ? green : red);
        for (int i = 0; i < 2; ++i) {
            const auto& h = f.halves[static_cast<std::size_t>(i)];
            std::string s = std::string(h.control ? (h.control->crc_ok ? "✓ " : "✗ ") : "  ") + fit(half_name(h.kind), 6) + (i ? "2nd " : "1st ");
            if (h.control) s += to_hex(h.control->message) + "  path errors " + std::to_string(h.control->bit_errors);
            else s += to_hex(h.bits);
            add(s, h.control ? (h.control->crc_ok ? green : red) : normal);
        }
    } else add("Waiting for the sync word CDF59h...", dim);
    horizontal("MESSAGE LOG (newest first)", "├", "┤");
    const int reserved = 2;   // log rows below must leave the bottom border visible
    const int free_rows = height - static_cast<int>(lines.size()) - reserved;
    if (log_.empty()) add("--", dim);
    for (int i = 0; i < std::max(1, free_rows) && i < static_cast<int>(log_.size()); ++i)
        add(log_[static_cast<std::size_t>(i)], i == 0 ? white : normal);
    horizontal("Ctrl+C: stop and save", "└", "┘");

    std::cerr << "\x1b[H";
    for (std::size_t i = 0; i < lines.size() && i < static_cast<std::size_t>(height); ++i) {
        std::cerr << lines[i].color << lines[i].text << "\x1b[0m\x1b[K";
        if (i + 1 < lines.size() && i + 1 < static_cast<std::size_t>(height)) std::cerr << '\n';
    }
    std::cerr << "\x1b[J" << std::flush;
}
}
