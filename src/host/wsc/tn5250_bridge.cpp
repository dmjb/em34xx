// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/wsc/tn5250_bridge.hpp"

#include "util/log.hpp"
#include "util/strings.hpp"

#include <algorithm>

namespace em34xx {
namespace wsc {

Tn5250Config station_session_config(const Tn5250Config& tn5250, const StationConfig& station, bool first_display,
                                    const std::string& model, DisplaySubtype display) {
    Tn5250Config c = tn5250;   // host, port, TLS settings, code page, environment
    c.device_name = !station.device_name.empty() ? station.device_name
                    : first_display              ? tn5250.device_name
                                                 : std::string();
    c.terminal_type = station_terminal_type(tn5250, station, model, display);
    apply_station_tls(c, station);   // per-station TLS versions and pinned certificate
    if (!station.host.empty()) c.host = station.host;
    if (station.port) c.port = station.port;
    return c;
}

// ---------------------------------------------------------------------------
// DisplayStationPort

namespace {
tn5250::Client::TransportFactory transport_for(const BridgeOptions& opt, const StationConfig& st) {
    if (!opt.transport) return nullptr;
    auto make = opt.transport;
    return [make, st](std::string& error) { return make(st, error); };
}
} // namespace

DisplayStationPort::DisplayStationPort(StationConfig station, Tn5250Config cfg, const BridgeOptions& opt,
                                       Station& link)
    : station_(std::move(station)), cfg_(std::move(cfg)), opt_(opt), link_(link) {
    new_client();
    link_.set_plugged(false, 0);   // no cable until the session is up
    // Connect now, not on the first advance(): the link device only calls
    // in once the firmware runs its Twinax receiver.
    start_session();
}

DisplayStationPort::~DisplayStationPort() {
    if (client_) client_->stop();
}

void DisplayStationPort::pump() {
    if (client_) client_->pump();
}

void DisplayStationPort::device_ready(const StationIdentity& id) {
    online_ = true;
    adapter_->device_ready(id);
    client_->set_device_ready(true);   // repaints the whole screen after a loss
}

void DisplayStationPort::device_lost() {
    online_ = false;
    adapter_->device_lost();
    client_->set_device_ready(false);
}

uint64_t DisplayStationPort::screen_generation() const { return generation_base_ + client_->generation(); }

void DisplayStationPort::screen_snapshot(ScreenState& out) const { adapter_->screen_snapshot(out); }

bool DisplayStationPort::take_alarm() { return adapter_->take_alarm(); }

void DisplayStationPort::operator_action(const OperatorAction& action) { adapter_->operator_action(action); }
void DisplayStationPort::pointer_action(const PointerAction& action) { adapter_->pointer_action(action); }

bool DisplayStationPort::connected() const { return client_->state() == tn5250::ConnState::Ready; }

void DisplayStationPort::new_client() {
    client_ = std::make_unique<tn5250::Client>(cfg_, transport_for(opt_, station_));
    if (opt_.tls_trust) {
        const tn5250::TlsTrustHandler ask = opt_.tls_trust;
        const unsigned address = station_.address;
        client_->set_tls_trust_handler([ask, address](const tn5250::TlsTrustRequest& req) {
            tn5250::TlsTrustRequest r = req;
            r.station = address;
            return ask(r);
        });
    }
    adapter_ = std::make_unique<Tn5250DisplayAdapter>(*client_);
}

void DisplayStationPort::start_session() {
    if (client_->state() != tn5250::ConnState::Idle) {
        // A used client cannot be restarted: replace it. The generation
        // base keeps screen_generation() moving forward.
        generation_base_ += client_->generation() + 1;
        client_->stop();
        new_client();
    }
    ++starts_;
    last_state_ = tn5250::ConnState::Connecting;
    LOG_INFO("tn5250", "station %u: connecting to %s:%u%s, terminal type %s, device %s", station_.address,
             cfg_.host.c_str(), cfg_.port, cfg_.tls ? " (TLS)" : "", cfg_.terminal_type.c_str(),
             cfg_.device_name.empty() ? "(host assigned)" : cfg_.device_name.c_str());
    std::string error;
    if (!client_->start(error)) LOG_ERROR("tn5250", "station %u: %s", station_.address, error.c_str());
}

void DisplayStationPort::hold() {
    if (held_) return;
    held_ = true;
    LOG_INFO("tn5250", "station %u: disconnecting", station_.address);
    client_->stop();
}

void DisplayStationPort::tick(uint64_t now_ns) {
    using tn5250::ConnState;
    ConnState st = client_->state();
    if (st == ConnState::Idle && !held_) {
        start_session();
        st = client_->state();
    }
    if (st != last_state_) {
        if (st == ConnState::Ready) {
            LOG_INFO("tn5250", "station %u: session connected", station_.address);
            backoff_ms_ = 0;
        } else if ((st == ConnState::Closed || st == ConnState::Failed) && !held_) {
            const std::string err = client_->last_error();
            backoff_ms_ = backoff_ms_ ? std::min(backoff_ms_ * 2, opt_.reconnect_max_ms) : opt_.reconnect_min_ms;
            retry_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(backoff_ms_);
            LOG_WARN("tn5250", "station %u: session %s%s%s%s", station_.address, tn5250::conn_state_name(st),
                     err.empty() ? "" : ": ", err.c_str(),
                     opt_.reconnect ? format("; reconnecting in %u ms", backoff_ms_).c_str() : "");
        }
        last_state_ = st;
    }
    // The session is the cable.
    link_.set_plugged(st == ConnState::Ready, now_ns);

    if (!held_ && opt_.reconnect && (st == ConnState::Closed || st == ConnState::Failed) &&
        std::chrono::steady_clock::now() >= retry_at_)
        start_session();
}

std::string DisplayStationPort::describe() const {
    std::string out = format("station %u display: cable %s, session %s%s to %s:%u, TLS %s, type %s, device %s, "
                             "terminal %s, %u connection(s)",
                             station_.address, link_.plugged() ? "plugged" : "unplugged",
                             tn5250::conn_state_name(client_->state()), held_ ? " (disconnected)" : "",
                             cfg_.host.c_str(), cfg_.port, cfg_.tls ? "yes" : "no", cfg_.terminal_type.c_str(),
                             cfg_.device_name.empty() ? "(host assigned)" : cfg_.device_name.c_str(),
                             online_ ? "on line" : "not on line", starts_);
    const std::string err = client_->last_error();
    if (!err.empty()) out += ", last error: " + err;
    return out + "\n";
}

// ---------------------------------------------------------------------------
// Printer stub

void NullPrinterSession::set_device_ready(bool ready) {
    if (!ready || logged_) return;
    logged_ = true;
    LOG_WARN("tn5250", "station %u: printer found, but TN5250 printer sessions are not implemented yet; "
                       "it is polled for status and receives no data",
             station_.address);
}

void PrinterStationPort::device_ready(const StationIdentity& id) {
    ready_ = true;
    LOG_INFO("wsc", "station %u: printer type %s", id.address, printer_type_name(id.device_id));
    session_->set_device_ready(true);
}

void PrinterStationPort::device_lost() {
    ready_ = false;
    session_->set_device_ready(false);
}

bool PrinterStationPort::next_print_data(std::vector<uint8_t>& scs) {
    if (!session_->take_scs(scs)) return false;
    session_->acknowledge();   // TODO(printer): acknowledge on the printer's print-complete status
    return true;
}

void PrinterStationPort::printer_status(uint8_t byte2) {
    status_ = byte2;
    session_->report_status(byte2);
}

// ---------------------------------------------------------------------------
// Tn5250Bridge

Tn5250Bridge::Tn5250Bridge(const TwinaxConfig& twinax, const Tn5250Config& tn5250, BridgeOptions opt)
    : opt_(std::move(opt)), tn5250_(tn5250), sched_(twinax.effective_stations(), opt_.timing) {
    bool first_display = true;
    for (const StationConfig& sc : twinax.effective_stations()) {
        Station* s = sched_.station(uint8_t(sc.address));
        if (sc.type == StationType::Display) {
            auto port = std::make_unique<DisplayStationPort>(
                sc, station_session_config(tn5250, sc, first_display, opt_.model, opt_.display), opt_, *s);
            first_display = false;
            s->attach_display(port.get());
            displays_.push_back(std::move(port));
        } else {
            auto port = std::make_unique<PrinterStationPort>(std::make_unique<NullPrinterSession>(sc));
            s->attach_printer(port.get());
            s->set_plugged(false, 0);
            printers_.emplace_back(uint8_t(sc.address), std::move(port));
        }
    }
}

Tn5250Bridge::~Tn5250Bridge() {
    // Stations hold raw pointers to the ports; detach before the ports go.
    for (auto& d : displays_) sched_.station(uint8_t(d->station().address))->attach_display(nullptr);
    for (auto& p : printers_) sched_.station(p.first)->attach_printer(nullptr);
}

void Tn5250Bridge::advance(uint64_t emu_ns) {
    sched_.advance(emu_ns);   // pumps every display session
    bool any_connected = false;
    for (auto& d : displays_) {
        d->tick(sched_.now());
        any_connected = any_connected || d->connected();
    }
    // Printers share the cable: polled while any display session is up.
    for (auto& p : printers_) {
        sched_.station(p.first)->set_plugged(any_connected, sched_.now());
        p.second->pump();
    }
}

bool Tn5250Bridge::hold(uint8_t address) {
    DisplayStationPort* d = display(address);
    if (!d) return false;
    d->hold();
    d->tick(sched_.now());
    return true;
}

DisplayStationPort* Tn5250Bridge::display(uint8_t address) {
    for (auto& d : displays_)
        if (d->station().address == address) return d.get();
    return nullptr;
}

PrinterStationPort* Tn5250Bridge::printer(uint8_t address) {
    for (auto& p : printers_)
        if (p.first == address) return p.second.get();
    return nullptr;
}

std::string Tn5250Bridge::describe() const {
    std::string out = "twinax: TN5250 bridge\n";
    out += sched_.describe();
    for (const auto& d : displays_) out += "  " + d->describe();
    for (const auto& p : printers_)
        out += format("  station %u printer: cable %s (with the display sessions), session not implemented "
                      "(placeholder), %s, status %02X\n",
                      p.first, sched_.station(p.first)->plugged() ? "plugged" : "unplugged",
                      p.second->ready() ? "on line" : "not on line", p.second->last_status());
    return out;
}

std::vector<TwinaxSessionStatus> Tn5250Bridge::session_status() const {
    std::vector<TwinaxSessionStatus> out;
    for (const auto& d : displays_) {
        TwinaxSessionStatus st;
        st.address = d->station().address;
        const tn5250::Client* c = d->client();
        st.connected = d->connected();
        st.held = d->held();
        st.state = d->held() ? "disconnected" : tn5250::conn_state_name(c->state());
        st.peer = format("%s:%u", d->config().host.c_str(), d->config().port);
        st.tls = d->config().tls;
        st.error = c->last_error();
        out.push_back(st);
    }
    for (const auto& p : printers_) {
        TwinaxSessionStatus st;
        st.address = p.first;
        st.printer = true;
        st.state = p.second->ready() ? "on line (no session)" : "not on line";
        out.push_back(st);
    }
    return out;
}

std::unique_ptr<TwinaxHost> make_tn5250_bridge(const TwinaxConfig& twinax, const Tn5250Config& tn5250,
                                               BridgeOptions opt, std::string& error) {
    error = twinax.validate();
    if (!error.empty()) return nullptr;
    error = tn5250.validate();
    if (!error.empty()) return nullptr;
    if (!tn5250.configured()) {
        error = "twinax host 'tn5250' needs a tn5250 host name";
        return nullptr;
    }
    return std::make_unique<Tn5250Bridge>(twinax, tn5250, std::move(opt));
}

} // namespace wsc
} // namespace em34xx
