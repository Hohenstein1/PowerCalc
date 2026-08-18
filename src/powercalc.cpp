#include <algorithm>
#include <csignal>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr const char* kVersion = "0.3.0";
constexpr const char* kAuthor = "Hohenstein256";

static volatile std::sig_atomic_t gStopRequested = 0;

static void request_stop(int) {
    gStopRequested = 1;
}

class CachedFile {
public:
    CachedFile() = default;
    explicit CachedFile(const fs::path& path) : path_(path), fd_(::open(path.c_str(), O_RDONLY | O_CLOEXEC)) {}

    ~CachedFile() {
        if (fd_ >= 0) ::close(fd_);
    }

    CachedFile(const CachedFile&) = delete;
    CachedFile& operator=(const CachedFile&) = delete;

    CachedFile(CachedFile&& other) noexcept : path_(std::move(other.path_)), fd_(std::exchange(other.fd_, -1)) {}
    CachedFile& operator=(CachedFile&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            path_ = std::move(other.path_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    [[nodiscard]] bool valid() const { return fd_ >= 0; }
    [[nodiscard]] const fs::path& path() const { return path_; }

    [[nodiscard]] std::optional<std::string> text() const {
        if (fd_ < 0) return std::nullopt;
        if (::lseek(fd_, 0, SEEK_SET) < 0) return std::nullopt;

        char buf[256];
        const ssize_t n = ::read(fd_, buf, sizeof(buf) - 1);
        if (n <= 0) return std::nullopt;
        buf[n] = '\0';

        std::string s(buf, static_cast<std::size_t>(n));
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) {
            s.pop_back();
        }
        return s;
    }

    [[nodiscard]] std::optional<double> number() const {
        auto s = text();
        if (!s) return std::nullopt;
        char* end = nullptr;
        errno = 0;
        const double value = std::strtod(s->c_str(), &end);
        if (errno != 0 || end == s->c_str() || !std::isfinite(value)) return std::nullopt;
        while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
        if (*end != '\0') return std::nullopt;
        return value;
    }

private:
    fs::path path_;
    mutable int fd_{-1};
};

struct PowerReading {
    std::string source;
    std::string label;
    double watts{};
    bool total_like{};
    bool battery{};
};

struct Stats {
    std::size_t count{};
    double sum{};
    double min{std::numeric_limits<double>::infinity()};
    double max{-std::numeric_limits<double>::infinity()};

    void add(double value) {
        if (!std::isfinite(value)) return;
        ++count;
        sum += value;
        min = std::min(min, value);
        max = std::max(max, value);
    }

    [[nodiscard]] bool empty() const { return count == 0; }
    [[nodiscard]] double average() const { return empty() ? 0.0 : sum / static_cast<double>(count); }
};

struct PowerSupplySensor {
    std::string name;
    std::string type;
    CachedFile status;
    CachedFile power;
    CachedFile voltage;
    CachedFile current;
    bool use_direct_power{};

    [[nodiscard]] std::optional<double> watts() const {
        if (use_direct_power) {
            auto raw = power.number();
            if (!raw) return std::nullopt;
            return std::fabs(*raw) / 1e6;
        }
        auto v = voltage.number();
        auto c = current.number();
        if (!v || !c) return std::nullopt;
        return std::fabs(*v * *c) / 1e12;
    }

    [[nodiscard]] std::optional<PowerReading> read() const {
        const std::string state = status.text().value_or("");
        const bool is_battery = type == "Battery";
        const bool total_like = is_battery && state == "Discharging";

        auto value = watts();
        if (!value) return std::nullopt;
        std::string label = name;
        if (!use_direct_power) label += " (V*I estimate)";
        if (!state.empty()) label += " (" + state + ")";
        return PowerReading{"power_supply", std::move(label), *value, total_like, is_battery};
    }
};

struct HwmonPowerSensor {
    std::string label;
    CachedFile power;
    bool amd_ppt{};

    [[nodiscard]] std::optional<double> watts() const {
        auto raw = power.number();
        if (!raw) return std::nullopt;
        return std::fabs(*raw) / 1e6;
    }

    [[nodiscard]] std::optional<PowerReading> read() const {
        auto value = watts();
        if (!value) return std::nullopt;
        return PowerReading{"hwmon", label, *value, false, false};
    }
};

struct EnergySensor {
    std::string source;
    std::string label;
    CachedFile energy;
    double scale_to_joules{1e-6};
    std::optional<double> max_raw;
    std::optional<double> previous_raw;
};

static std::optional<std::string> read_text_once(const fs::path& path) {
    CachedFile f(path);
    return f.text();
}

static std::optional<double> read_number_once(const fs::path& path) {
    CachedFile f(path);
    return f.number();
}

static std::string lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

static bool contains_ci(const std::string& haystack, const std::string& needle) {
    const auto h = lower_copy(haystack);
    const auto n = lower_copy(needle);
    return h.find(n) != std::string::npos;
}

static std::string human_watts(double watts) {
    std::ostringstream out;
    if (std::fabs(watts) < 1.0) out << std::fixed << std::setprecision(3) << watts << " W";
    else if (std::fabs(watts) < 100.0) out << std::fixed << std::setprecision(2) << watts << " W";
    else out << std::fixed << std::setprecision(1) << watts << " W";
    return out.str();
}

static std::string human_wh(double wh) {
    std::ostringstream out;
    if (std::fabs(wh) < 1.0) out << std::fixed << std::setprecision(4) << wh << " Wh";
    else out << std::fixed << std::setprecision(3) << wh << " Wh";
    return out.str();
}

static std::string basename_of(const fs::path& p) {
    return p.filename().string();
}

static std::vector<PowerSupplySensor> discover_power_supply() {
    std::vector<PowerSupplySensor> out;
    const fs::path root = "/sys/class/power_supply";
    std::error_code ec;
    if (!fs::exists(root, ec)) return out;

    for (const auto& entry : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        const auto dir = entry.path();
        const std::string name = basename_of(dir);
        const std::string type = read_text_once(dir / "type").value_or("unknown");

        CachedFile status(dir / "status");
        CachedFile power(dir / "power_now");
        if (power.valid()) {
            out.push_back({name, type, std::move(status), std::move(power), CachedFile{}, CachedFile{}, true});
            continue;
        }

        CachedFile voltage(dir / "voltage_now");
        CachedFile current(dir / "current_now");
        if (voltage.valid() && current.valid()) {
            out.push_back({name, type, std::move(status), CachedFile{}, std::move(voltage), std::move(current), false});
        }
    }
    return out;
}

static std::vector<HwmonPowerSensor> discover_hwmon_power() {
    std::vector<HwmonPowerSensor> out;
    const fs::path root = "/sys/class/hwmon";
    std::error_code ec;
    if (!fs::exists(root, ec)) return out;

    for (const auto& entry : fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) break;
        const auto dir = entry.path();
        const std::string dev = read_text_once(dir / "name").value_or(basename_of(dir));

        std::error_code inner_ec;
        for (const auto& file : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, inner_ec)) {
            if (inner_ec) break;
            const std::string fn = file.path().filename().string();
            if (fn.rfind("power", 0) != 0) continue;

            const bool is_input = fn.size() > 6 && fn.ends_with("_input");
            const bool is_average = fn.size() > 8 && fn.ends_with("_average");
            if (!is_input && !is_average) continue;

            std::string idx;
            for (std::size_t i = 5; i < fn.size() && std::isdigit(static_cast<unsigned char>(fn[i])); ++i) {
                idx.push_back(fn[i]);
            }

            std::string sensor_label;
            if (!idx.empty()) {
                sensor_label = read_text_once(dir / ("power" + idx + "_label")).value_or("power" + idx);
            }

            std::string label = dev;
            if (!sensor_label.empty()) label += " / " + sensor_label;

            const bool amd_ppt = contains_ci(dev, "amdgpu") && contains_ci(sensor_label, "ppt");
            if (amd_ppt) {
                label = "AMD SoC / " + sensor_label;
                label += is_average ? " (average; may include CPU)" : " (instantaneous; may include CPU)";
            } else {
                label += is_average ? " (average)" : " (instantaneous)";
            }

            CachedFile power(file.path());
            if (power.valid()) out.push_back({std::move(label), std::move(power), amd_ppt});
        }
    }
    return out;
}

static std::vector<EnergySensor> discover_energy_counters() {
    std::vector<EnergySensor> out;
    std::set<std::string> seen;
    const fs::path powercap = "/sys/class/powercap";
    std::error_code ec;

    auto add_powercap = [&](const fs::path& file) {
        if (file.filename() != "energy_uj") return;
        std::error_code cec;
        const fs::path canonical = fs::weakly_canonical(file, cec);
        if (cec) return;
        const std::string key = canonical.string();
        if (!seen.insert(key).second) return;

        CachedFile energy(canonical);
        if (!energy.valid()) return;
        const auto dir = canonical.parent_path();
        const std::string label = read_text_once(dir / "name").value_or(basename_of(dir));
        out.push_back({"powercap", label, std::move(energy), 1e-6,
                       read_number_once(dir / "max_energy_range_uj"), std::nullopt});
    };

    if (fs::exists(powercap, ec)) {
        for (const auto& root_entry : fs::directory_iterator(powercap, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            std::error_code cec;
            const fs::path physical = fs::weakly_canonical(root_entry.path(), cec);
            if (cec || !fs::is_directory(physical, cec)) continue;

            add_powercap(physical / "energy_uj");
            std::error_code rec_ec;
            for (auto it = fs::recursive_directory_iterator(physical, fs::directory_options::skip_permission_denied, rec_ec);
                 !rec_ec && it != fs::recursive_directory_iterator(); ++it) {
                std::error_code sec;
                if (it->is_regular_file(sec) || it->is_symlink(sec)) add_powercap(it->path());
            }
        }
    }

    const fs::path hwmon = "/sys/class/hwmon";
    ec.clear();
    if (fs::exists(hwmon, ec)) {
        for (const auto& entry : fs::directory_iterator(hwmon, fs::directory_options::skip_permission_denied, ec)) {
            if (ec) break;
            const auto dir = entry.path();
            const std::string dev = read_text_once(dir / "name").value_or(basename_of(dir));
            std::error_code inner_ec;
            for (const auto& file : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, inner_ec)) {
                if (inner_ec) break;
                const std::string fn = file.path().filename().string();
                if (fn.rfind("energy", 0) != 0 || !fn.ends_with("_input")) continue;

                std::string idx;
                for (std::size_t i = 6; i < fn.size() && std::isdigit(static_cast<unsigned char>(fn[i])); ++i) {
                    idx.push_back(fn[i]);
                }
                std::string label = dev;
                if (!idx.empty()) {
                    label += " / " + read_text_once(dir / ("energy" + idx + "_label")).value_or("energy" + idx);
                }

                CachedFile energy(file.path());
                if (energy.valid()) out.push_back({"hwmon-energy", std::move(label), std::move(energy), 1e-6, std::nullopt, std::nullopt});
            }
        }
    }
    return out;
}

static std::vector<PowerReading> read_power_supplies(const std::vector<PowerSupplySensor>& sensors, bool live_filter) {
    std::vector<PowerReading> out;
    out.reserve(sensors.size());
    for (const auto& sensor : sensors) {
        auto reading = sensor.read();
        if (!reading) continue;
        if (live_filter && !reading->battery && !reading->total_like && std::fabs(reading->watts) < 0.001) continue;
        out.push_back(std::move(*reading));
    }
    return out;
}

static std::vector<PowerReading> read_hwmon(const std::vector<HwmonPowerSensor>& sensors) {
    std::vector<PowerReading> out;
    out.reserve(sensors.size());
    for (const auto& sensor : sensors) {
        if (auto reading = sensor.read()) out.push_back(std::move(*reading));
    }
    return out;
}

static void initialize_energy(std::vector<EnergySensor>& sensors) {
    for (auto& sensor : sensors) sensor.previous_raw = sensor.energy.number();
}

static void print_readings(const std::vector<PowerReading>& readings) {
    if (readings.empty()) {
        std::cout << "  (none)\n";
        return;
    }
    for (const auto& reading : readings) {
        std::cout << "  " << std::left << std::setw(48) << reading.label
                  << " " << std::right << std::setw(10) << human_watts(reading.watts);
        if (reading.total_like) std::cout << "  [whole-system-like]";
        std::cout << '\n';
    }
}

static int cmd_scan() {
    auto power_supply = discover_power_supply();
    auto hwmon = discover_hwmon_power();
    auto energy = discover_energy_counters();

    std::cout << "PowerCalc " << kVersion << " sensor scan\n\n";
    std::cout << "power_supply:\n";
    print_readings(read_power_supplies(power_supply, false));
    std::cout << "\nhwmon power:\n";
    print_readings(read_hwmon(hwmon));

    std::cout << "\nenergy counters:\n";
    if (energy.empty()) {
        std::cout << "  (none)\n";
    } else {
        for (const auto& sensor : energy) {
            std::cout << "  " << sensor.source << "  " << sensor.label << "  \"" << sensor.energy.path() << "\"\n";
        }
    }

    std::cout << "\nNote: readings may overlap. Do not sum battery, CPU package, core and AMD SoC/PPT values.\n";
    return 0;
}

static void print_stat_block(const std::string& title, const Stats& stats) {
    if (stats.empty()) return;
    std::cout << title << '\n';
    std::cout << "  Average       " << std::setw(10) << human_watts(stats.average()) << '\n';
    std::cout << "  Minimum       " << std::setw(10) << human_watts(stats.min) << '\n';
    std::cout << "  Maximum       " << std::setw(10) << human_watts(stats.max) << '\n';
}

static std::optional<double> read_battery_total(const std::vector<PowerSupplySensor>& sensors) {
    for (const auto& sensor : sensors) {
        if (sensor.type != "Battery") continue;
        if (sensor.status.text().value_or("") != "Discharging") continue;
        auto value = sensor.watts();
        if (value && *value > 0.0) return value;
    }
    return std::nullopt;
}

static std::optional<double> read_amd_ppt_direct(const std::vector<HwmonPowerSensor>& sensors) {
    for (const auto& sensor : sensors) {
        if (!sensor.amd_ppt) continue;
        if (auto value = sensor.watts()) return value;
    }
    return std::nullopt;
}

static void sample_rapl(std::vector<EnergySensor>& sensors, double dt, Stats& package, Stats& core,
                        std::optional<double>& current_package) {
    current_package.reset();
    if (!(dt > 0.0)) return;

    for (auto& sensor : sensors) {
        auto current = sensor.energy.number();
        if (!current) {
            sensor.previous_raw.reset();
            continue;
        }
        if (!sensor.previous_raw) {
            sensor.previous_raw = current;
            continue;
        }

        double delta_raw = *current - *sensor.previous_raw;
        if (delta_raw < 0.0) {
            if (sensor.max_raw && *sensor.max_raw >= *sensor.previous_raw) {
                delta_raw = (*sensor.max_raw - *sensor.previous_raw) + *current;
            } else {
                sensor.previous_raw = current;
                continue;
            }
        }
        sensor.previous_raw = current;

        const double watts = (delta_raw * sensor.scale_to_joules) / dt;
        if (!std::isfinite(watts) || watts < 0.0) continue;

        const std::string lower = lower_copy(sensor.label);
        if (lower.rfind("package-", 0) == 0 || lower == "package") {
            package.add(watts);
            current_package = watts;
        } else if (lower == "core" || lower.rfind("core-", 0) == 0) {
            core.add(watts);
        }
    }
}

static std::string compact_value(const char* name, const std::optional<double>& value) {
    std::ostringstream out;
    out << name << '=';
    if (value) out << human_watts(*value);
    else out << "n/a";
    return out.str();
}

static int cmd_live(double interval, int count, std::optional<double> price_per_kwh) {
    interval = std::max(interval, 0.1);
    count = std::max(count, 1);

    auto power_supply = discover_power_supply();
    auto hwmon = discover_hwmon_power();
    auto energy = discover_energy_counters();
    initialize_energy(energy);

    Stats whole_system;
    Stats cpu_package;
    Stats cpu_core;
    Stats amd_soc_ppt;

    std::optional<double> previous_total = read_battery_total(power_supply);
    auto previous_time = Clock::now();
    const auto session_start = previous_time;
    double measured_wh = 0.0;

    const bool interactive = ::isatty(STDOUT_FILENO) != 0;
    gStopRequested = 0;
    const auto old_sigint = std::signal(SIGINT, request_stop);
    const auto old_sigterm = std::signal(SIGTERM, request_stop);
    int completed_samples = 0;

    if (interactive) {
        std::cout << "PowerCalc live: " << count << " samples @ " << std::fixed << std::setprecision(2)
                  << interval << " s";
        if (price_per_kwh) std::cout << ", price " << *price_per_kwh << "/kWh";
        std::cout << "\n";
    }

    for (int i = 0; i < count && !gStopRequested; ++i) {
        const auto deadline = session_start + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(interval * static_cast<double>(i + 1)));
        std::this_thread::sleep_until(deadline);
        if (gStopRequested) break;

        const auto now = Clock::now();
        const double dt = std::chrono::duration<double>(now - previous_time).count();
        previous_time = now;

        const auto total = read_battery_total(power_supply);
        const auto amd = read_amd_ppt_direct(hwmon);
        std::optional<double> package_now;
        sample_rapl(energy, dt, cpu_package, cpu_core, package_now);

        if (total) whole_system.add(*total);
        if (amd) amd_soc_ppt.add(*amd);

        if (previous_total && total) measured_wh += ((*previous_total + *total) * 0.5) * dt / 3600.0;
        else if (total) measured_wh += *total * dt / 3600.0;
        previous_total = total;
        ++completed_samples;

        if (interactive) {
            const double elapsed = std::chrono::duration<double>(now - session_start).count();
            std::ostringstream line;
            line << "\r\033[2K[" << completed_samples << '/' << count << "] "
                 << compact_value("System", total) << " | "
                 << compact_value("CPU", package_now) << " | "
                 << compact_value("AMD", amd)
                 << " | Energy=" << human_wh(measured_wh)
                 << " | " << std::fixed << std::setprecision(1) << elapsed << " s";
            std::cout << line.str() << std::flush;
        }
    }

    std::signal(SIGINT, old_sigint);
    std::signal(SIGTERM, old_sigterm);

    if (interactive) std::cout << "\n";

    const double elapsed_seconds = std::chrono::duration<double>(Clock::now() - session_start).count();
    std::cout << "------------------------------------------------------------\n";
    std::cout << completed_samples << "-sample summary (" << std::fixed << std::setprecision(2) << elapsed_seconds << " s)";
    if (gStopRequested) std::cout << " [stopped]";
    std::cout << "\n\n";

    if (!whole_system.empty()) {
        print_stat_block("Whole system (battery)", whole_system);
        std::cout << "  Measured energy " << std::setw(10) << human_wh(measured_wh) << "\n\n";
    }
    if (!cpu_package.empty()) {
        print_stat_block("CPU package (RAPL)", cpu_package);
        std::cout << '\n';
    }
    if (!cpu_core.empty()) {
        print_stat_block("CPU core domain (nested inside package)", cpu_core);
        std::cout << "  Note: do not add this value to CPU package.\n\n";
    }
    if (!amd_soc_ppt.empty()) {
        print_stat_block("AMD SoC / PPT", amd_soc_ppt);
        std::cout << "  Note: PPT may include CPU power on an APU; direct readings may be instantaneous.\n\n";
    }

    if (!whole_system.empty()) {
        const double avg = elapsed_seconds > 0.0 && measured_wh > 0.0
            ? (measured_wh * 3600.0 / elapsed_seconds)
            : whole_system.average();
        std::cout << "Primary estimate: " << human_watts(avg) << " average whole-system draw (battery-based)\n";
        std::cout << "Energy measured:  " << human_wh(measured_wh) << " over this session\n";

        if (price_per_kwh) {
            const double measured_cost = (measured_wh / 1000.0) * *price_per_kwh;
            const double hourly_cost = (avg / 1000.0) * *price_per_kwh;
            const double daily_cost = hourly_cost * 24.0;
            const double yearly_cost = daily_cost * 365.0;
            std::cout << std::fixed << std::setprecision(6)
                      << "Measured cost:    " << measured_cost << " (same currency as price input)\n";
            std::cout << std::setprecision(4)
                      << "At this average:  " << hourly_cost << "/hour, " << daily_cost << "/day, "
                      << yearly_cost << "/year\n";
        }
    } else {
        std::cout << "Primary estimate: unavailable (no discharging battery total detected).\n";
        std::cout << "Energy measured:  unavailable without a whole-system-like source.\n";
    }
    std::cout << "------------------------------------------------------------\n";
    return 0;
}

static std::optional<double> parse_number(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::nullopt;
    s.erase(0, first);
    const auto last = s.find_last_not_of(" \t\r\n");
    s.erase(last + 1);

    if (s.find('.') == std::string::npos && std::count(s.begin(), s.end(), ',') == 1) {
        std::replace(s.begin(), s.end(), ',', '.');
    }

    try {
        std::size_t pos = 0;
        const double value = std::stod(s, &pos);
        if (pos != s.size() || !std::isfinite(value)) return std::nullopt;
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

static int cmd_cost(int argc, char** argv) {
    if (argc < 4) {
        std::cerr << "Usage: powercalc cost <watts> <price_per_kWh> [hours_per_day=24] [days=365]\n";
        return 2;
    }
    auto watts = parse_number(argv[2]);
    auto price = parse_number(argv[3]);
    auto hours = argc >= 5 ? parse_number(argv[4]) : std::optional<double>(24.0);
    auto days = argc >= 6 ? parse_number(argv[5]) : std::optional<double>(365.0);
    if (!watts || !price || !hours || !days || *watts < 0 || *price < 0 || *hours < 0 || *hours > 24 || *days < 0) {
        std::cerr << "Invalid numeric value.\n";
        return 2;
    }
    const double kwh = (*watts / 1000.0) * *hours * *days;
    const double cost = kwh * *price;
    std::cout << std::fixed << std::setprecision(3) << "Energy: " << kwh << " kWh\n";
    std::cout << std::setprecision(2) << "Cost:   " << cost << " (same currency as price input)\n";
    return 0;
}

static int cmd_estimate(int argc, char** argv) {
    if (argc < 6) {
        std::cerr << "Usage: powercalc estimate <rated_watts> <load_percent> <hours_per_day> <price_per_kWh> [days=365]\n";
        return 2;
    }
    auto rated = parse_number(argv[2]);
    auto load = parse_number(argv[3]);
    auto hours = parse_number(argv[4]);
    auto price = parse_number(argv[5]);
    auto days = argc >= 7 ? parse_number(argv[6]) : std::optional<double>(365.0);
    if (!rated || !load || !hours || !price || !days || *rated < 0 || *load < 0 || *load > 100 ||
        *hours < 0 || *hours > 24 || *price < 0 || *days < 0) {
        std::cerr << "Invalid numeric value.\n";
        return 2;
    }
    const double average_watts = *rated * (*load / 100.0);
    const double kwh = (average_watts / 1000.0) * *hours * *days;
    std::cout << std::fixed << std::setprecision(2)
              << "Estimated average power: " << average_watts << " W\n"
              << "Estimated energy:        " << kwh << " kWh\n"
              << "Estimated cost:          " << (kwh * *price) << " (same currency as price input)\n";
    return 0;
}

static void usage() {
    std::cout
        << "PowerCalc - Linux power sensor and electricity cost utility\n"
        << "Version " << kVersion << " - " << kAuthor << "\n\n"
        << "Usage:\n"
        << "  powercalc scan\n"
        << "  powercalc live [interval_seconds=1] [count=60] [price_per_kWh]\n"
        << "  powercalc cost <watts> <price_per_kWh> [hours_per_day=24] [days=365]\n"
        << "  powercalc estimate <rated_watts> <load_percent> <hours_per_day> <price_per_kWh> [days=365]\n\n"
        << "Examples:\n"
        << "  powercalc scan\n"
        << "  powercalc live\n"
        << "  powercalc live 1 60\n"
        << "  powercalc live 1 60 0.23\n"
        << "  powercalc cost 80 0.23 6 30\n"
        << "  powercalc estimate 500 35 8 0.23\n\n"
        << "Live mode uses one refreshing terminal line and prints only the final summary.\n"
        << "When stdout is redirected, only the final summary is printed.\n"
        << "Press Ctrl+C during live mode to stop early and keep the summary.\n\n"
        << "Notes:\n"
        << "  - Linux sensor values can overlap; do not sum them blindly.\n"
        << "  - A discharging battery is the preferred whole-system approximation on a laptop.\n"
        << "  - AMD PPT on an APU may represent SoC power and may include CPU power.\n"
        << "  - Desktop wall power usually cannot be known without suitable hardware telemetry.\n";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 0;
    }

    const std::string cmd = argv[1];
    try {
        if (cmd == "scan") return cmd_scan();
        if (cmd == "live") {
            double interval = 1.0;
            int count = 60;
            std::optional<double> price;

            if (argc >= 3) {
                auto value = parse_number(argv[2]);
                if (!value || *value <= 0.0) {
                    std::cerr << "Invalid interval.\n";
                    return 2;
                }
                interval = *value;
            }
            if (argc >= 4) {
                auto value = parse_number(argv[3]);
                if (!value || *value < 1.0 || *value > static_cast<double>(std::numeric_limits<int>::max()) ||
                    std::floor(*value) != *value) {
                    std::cerr << "Invalid count.\n";
                    return 2;
                }
                count = static_cast<int>(*value);
            }
            if (argc >= 5) {
                auto value = parse_number(argv[4]);
                if (!value || *value < 0.0) {
                    std::cerr << "Invalid price_per_kWh.\n";
                    return 2;
                }
                price = *value;
            }
            if (argc > 5) {
                std::cerr << "Too many arguments for live.\n";
                return 2;
            }
            return cmd_live(interval, count, price);
        }
        if (cmd == "cost") return cmd_cost(argc, argv);
        if (cmd == "estimate") return cmd_estimate(argc, argv);
        if (cmd == "-h" || cmd == "--help" || cmd == "help") {
            usage();
            return 0;
        }
        if (cmd == "-v" || cmd == "--version" || cmd == "version") {
            std::cout << "PowerCalc " << kVersion << " by " << kAuthor << '\n';
            return 0;
        }

        std::cerr << "Unknown command: " << cmd << "\n\n";
        usage();
        return 2;
    } catch (const fs::filesystem_error& e) {
        std::cerr << "Filesystem error: " << e.what() << '\n';
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
