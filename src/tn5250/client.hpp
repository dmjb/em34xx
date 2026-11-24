// SPDX-License-Identifier: GPL-3.0-or-later
// A TN5250 display session driven by Tn5250Config: transport + telnet
// negotiation on an I/O thread, the 5250 engine on the caller's thread.
//
// Records cross between the two threads through mutex-protected queues.
// The owner calls pump() (DisplaySession) from its own thread to apply
// what has arrived; replies the engine produces are queued and sent by the
// I/O thread.
//
// Both queues and the log are bounded. While kMaxQueuedBytes of host records
// wait for pump() the I/O thread stops reading the socket; replies beyond
// kMaxQueuedBytes fail the session; the log keeps its newest kMaxLogLines.
#pragma once

#include "host/host_config.hpp"
#include "tn5250/engine.hpp"
#include "tn5250/telnet.hpp"
#include "tn5250/transport.hpp"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace em34xx {
namespace tn5250 {

enum class ConnState { Idle, Connecting, Negotiating, Ready, Closed, Failed };
const char* conn_state_name(ConnState s);

// NEW-ENVIRON / TERMINAL-TYPE settings for a configuration. Sends DEVNAME
// only when configured, KBDTYPE/CODEPAGE/CHARSET when known for the code
// page (or given in env), then the remaining env entries; RFC 1572's
// standard names (USER, JOB, ACCT, PRINTER, SYSTEMTYPE, DISPLAY) go as
// VAR, everything else as USERVAR.
TelnetSettings telnet_settings_for(const Tn5250Config& cfg);

class Client : public DisplaySession {
public:
    using TransportFactory = std::function<std::unique_ptr<Transport>(std::string& error)>;
    static constexpr size_t kMaxQueuedBytes = 4u << 20;
    static constexpr size_t kMaxLogLines = 500;

    explicit Client(Tn5250Config cfg, TransportFactory factory = nullptr);
    ~Client() override;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Who decides about a TLS certificate the configuration refuses
    // (tls_trust.hpp); called on the I/O thread. Set before start().
    void set_tls_trust_handler(TlsTrustHandler fn) { trust_ = std::move(fn); }

    // Validate the configuration and start the I/O thread.
    bool start(std::string& error);
    void stop();
    ConnState state() const { return state_.load(); }
    std::string last_error() const;
    // Negotiation and record log lines since the last call (the newest
    // kMaxLogLines of them, after a note of how many were dropped).
    std::vector<std::string> take_log();

    Engine& engine() { return engine_; }
    const Engine& engine() const { return engine_; }

    // DisplaySession: everything but pump() forwards to the engine.
    void pump() override;
    int rows() const override { return engine_.rows(); }
    int cols() const override { return engine_.cols(); }
    const Cell& cell(int r, int c) const override { return engine_.cell(r, c); }
    int cursor_row() const override { return engine_.cursor_row(); }
    int cursor_col() const override { return engine_.cursor_col(); }
    Indicators indicators() const override { return engine_.indicators(); }
    uint64_t generation() const override { return engine_.generation(); }
    Rect take_dirty() override { return engine_.take_dirty(); }
    unsigned take_alarms() override { return engine_.take_alarms(); }
    void snapshot(ScreenSnapshot& out) const override { engine_.snapshot(out); }
    KeyResult key_data(uint8_t ebcdic) override { return engine_.key_data(ebcdic); }
    KeyResult key_function(Key key) override { return engine_.key_function(key); }
    KeyResult key_aid(Aid aid) override { return engine_.key_aid(aid); }
    KeyResult pointer_event(const PointerEvent& ev) override { return engine_.pointer_event(ev); }
    void set_record_sink(RecordSink sink) override;
    void set_device_ready(bool ready) override { engine_.set_device_ready(ready); }

private:
    void run();
    void run_session();
    void fail(const std::string& why);
    void log(const std::string& line);
    void log_locked(const std::string& line);
    void queue_outbound(const Record& rec);

    Tn5250Config cfg_;
    TransportFactory factory_;
    TlsTrustHandler trust_;
    Engine engine_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<ConnState> state_{ConnState::Idle};

    mutable std::mutex mu_;
    std::deque<Record> inbound_;
    std::deque<std::vector<uint8_t>> outbound_;   // framed, ready to send
    size_t inbound_bytes_ = 0;
    size_t outbound_bytes_ = 0;
    bool outbound_overflow_ = false;
    std::deque<std::string> log_;
    size_t log_dropped_ = 0;
    std::string error_;
};

} // namespace tn5250
} // namespace em34xx
