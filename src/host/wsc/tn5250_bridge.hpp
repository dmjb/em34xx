// SPDX-License-Identifier: GPL-3.0-or-later
// The TN5250 bridge: the virtual workstation controller (LinkScheduler and
// one Station per configured address) with a TN5250 session behind each
// display station. This is the TwinaxHost behind `--twinax-host tn5250` and
// the monitor's `connect` command.
//
//   IBM i <-> tn5250::Client (I/O thread) <-> engine (pump) <-> DisplayStationPort
//         <-> Station <-> LinkScheduler <-> TwinaxHost <-> terminal link device
//
// Threading. Every TwinaxHost call arrives on the emulation thread. The
// client's socket and telnet negotiation run on the client's own I/O
// thread; records cross on the client's queues and are applied to the
// engine only in pump(), which advance() calls. The controller and the
// engine therefore run on the emulation thread only, in emulated time.
//
// The TN5250 session is the cable (docs/twinax-bridge.md):
//   - A display station's controller end is plugged in only while its
//     TN5250 session is connected (negotiation done). Session up: the
//     station is plugged and the terminal goes through its power-on /
//     discovery handshake and comes on line; the sign-on screen follows.
//   - Session down (host close, connection lost, failed connect,
//     `disconnect`): the station is unplugged. Nothing polls it, so the
//     terminal sees no controller, exactly as with a pulled cable.
//   - Reconnecting repeats the full plug-in sequence. Unless the station
//     is held (monitor `disconnect`), the bridge reconnects after a
//     wall-clock back-off (the host is a real network peer), doubling from
//     `reconnect_min_ms` to `reconnect_max_ms`.
//   - The terminal itself going away while the session is up (no response,
//     power cycle) does not end the session: the engine is told, and when
//     the device returns the whole screen is repainted.
//
// Printer stations are discovered, identified and polled while at least
// one display session of the bridge is connected (the cable is shared), and
// get a placeholder printer session (NullPrinterSession) that accepts
// nothing and logs once that printer sessions are not implemented. A real
// tn5250::PrinterSession plugs in through PrinterStationPort.
#pragma once

#include "host/host_config.hpp"
#include "host/twinax_link.hpp"
#include "host/wsc/link_scheduler.hpp"
#include "host/wsc/session_port.hpp"
#include "host/wsc/tn5250_adapter.hpp"
#include "tn5250/client.hpp"
#include "tn5250/session_api.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace em34xx {
namespace wsc {

struct BridgeOptions {
    std::string model = "3477";                     // for the derived terminal type
    DisplaySubtype display = DisplaySubtype::Color;
    WscTiming timing;
    bool reconnect = true;                          // reconnect after the session ends
    unsigned reconnect_min_ms = 2000;               // wall clock
    unsigned reconnect_max_ms = 60000;
    // Asked when a session's TLS certificate is refused by the configuration
    // (tn5250/tls_trust.hpp); the request carries the station address.
    // Called on the session's I/O thread. Unset: the configuration decides.
    tn5250::TlsTrustHandler tls_trust;
    // Tests replace the network: one transport per session start.
    std::function<std::unique_ptr<tn5250::Transport>(const StationConfig&, std::string& error)> transport;
};

// The Tn5250Config one display station's session uses: the shared settings
// (host, port, TLS, code page, environment) with the station's own device
// name (the first display falls back to tn5250.device_name), terminal
// type (station_terminal_type), and its host / port / pinned certificate
// when it overrides them (a session to another system).
Tn5250Config station_session_config(const Tn5250Config& tn5250, const StationConfig& station, bool first_display,
                                    const std::string& model, DisplaySubtype display);

// A display station's DisplaySessionPort: owns the tn5250::Client (a fresh
// one per connection) and forwards to it through a Tn5250DisplayAdapter. It
// plugs and unplugs its Station with the session.
class DisplayStationPort : public DisplaySessionPort {
public:
    DisplayStationPort(StationConfig station, Tn5250Config cfg, const BridgeOptions& opt, Station& link);
    ~DisplayStationPort() override;

    // DisplaySessionPort.
    void pump() override;
    void device_ready(const StationIdentity& id) override;
    void device_lost() override;
    uint64_t screen_generation() const override;
    void screen_snapshot(ScreenState& out) const override;
    bool take_alarm() override;
    void operator_action(const OperatorAction& action) override;
    void pointer_action(const PointerAction& action) override;

    // Lifecycle, driven by Tn5250Bridge::advance() on the emulation thread.
    void tick(uint64_t now_ns);
    // Close the session and do not reconnect (monitor `disconnect N`).
    void hold();
    bool held() const { return held_; }

    const StationConfig& station() const { return station_; }
    const Tn5250Config& config() const { return cfg_; }
    tn5250::Client* client() { return client_.get(); }
    const tn5250::Client* client() const { return client_.get(); }
    bool connected() const;       // session negotiated: the cable is plugged
    bool online() const { return online_; }
    unsigned sessions_started() const { return starts_; }
    std::string describe() const;

private:
    void start_session();
    void new_client();

    StationConfig station_;
    Tn5250Config cfg_;
    const BridgeOptions& opt_;
    Station& link_;
    std::unique_ptr<tn5250::Client> client_;
    std::unique_ptr<Tn5250DisplayAdapter> adapter_;
    uint64_t generation_base_ = 0;    // keeps the generation monotonic across sessions
    bool online_ = false;
    bool held_ = false;
    unsigned starts_ = 0;
    unsigned backoff_ms_ = 0;
    std::chrono::steady_clock::time_point retry_at_{};
    tn5250::ConnState last_state_ = tn5250::ConnState::Idle;
};

// Placeholder printer session: accepts nothing, logs once.
// TODO(printer): replace with a TN5250 printer session (RFC 4777 sections
// 8-10, SCS data) behind the same tn5250::PrinterSession interface.
class NullPrinterSession : public tn5250::PrinterSession {
public:
    explicit NullPrinterSession(StationConfig station) : station_(std::move(station)) {}
    void pump() override {}
    bool take_scs(std::vector<uint8_t>&) override { return false; }
    void acknowledge() override {}
    void report_status(uint16_t) override {}
    void set_device_ready(bool ready) override;

private:
    StationConfig station_;
    bool logged_ = false;
};

// Connects a tn5250::PrinterSession to a printer station's PrinterSessionPort.
class PrinterStationPort : public PrinterSessionPort {
public:
    explicit PrinterStationPort(std::unique_ptr<tn5250::PrinterSession> session) : session_(std::move(session)) {}
    void device_ready(const StationIdentity& id) override;
    void device_lost() override;
    bool next_print_data(std::vector<uint8_t>& scs) override;
    void printer_status(uint8_t byte2) override;

    void pump() { session_->pump(); }
    bool ready() const { return ready_; }
    uint8_t last_status() const { return status_; }

private:
    std::unique_ptr<tn5250::PrinterSession> session_;
    bool ready_ = false;
    uint8_t status_ = 0;
};

class Tn5250Bridge : public TwinaxHost {
public:
    // Display sessions start connecting at once.
    Tn5250Bridge(const TwinaxConfig& twinax, const Tn5250Config& tn5250, BridgeOptions opt = BridgeOptions());
    ~Tn5250Bridge() override;

    const char* kind() const override { return "tn5250"; }
    void from_station(const TwinaxMessage& msg) override { sched_.from_station(msg); }
    bool to_station(TwinaxMessage& out) override { return sched_.to_station(out); }
    void no_response() override { sched_.no_response(); }
    void advance(uint64_t emu_ns) override;
    std::string describe() const override;
    bool disconnect_station(unsigned address) override { return address < 8 && hold(uint8_t(address)); }
    std::vector<TwinaxSessionStatus> session_status() const override;

    LinkScheduler& scheduler() { return sched_; }
    DisplayStationPort* display(uint8_t address);
    PrinterStationPort* printer(uint8_t address);
    // Close one display station's session and keep it closed; false if the
    // address is not a display station of this bridge.
    bool hold(uint8_t address);

private:
    BridgeOptions opt_;
    Tn5250Config tn5250_;
    LinkScheduler sched_;
    std::vector<std::unique_ptr<DisplayStationPort>> displays_;
    std::vector<std::pair<uint8_t, std::unique_ptr<PrinterStationPort>>> printers_;
};

// Validates the configuration and builds the bridge, or returns nullptr
// with `error` set.
std::unique_ptr<TwinaxHost> make_tn5250_bridge(const TwinaxConfig& twinax, const Tn5250Config& tn5250,
                                               BridgeOptions opt, std::string& error);

} // namespace wsc
} // namespace em34xx
