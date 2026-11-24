// SPDX-License-Identifier: GPL-3.0-or-later
// End to end through the TN5250 bridge: a scripted in-process TN5250 server
// -> tn5250::Client (its own I/O thread) -> engine -> station controller ->
// link scheduler -> FakeStation, and keystrokes back the same way.
#include "fake_station.hpp"
#include "tn5250_test_util.hpp"

#include "host/wsc/tn5250_bridge.hpp"
#include "tn5250/record.hpp"
#include "tn5250/telnet.hpp"
#include "tn5250/transport.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

using namespace em34xx;
using namespace em34xx::wsc;
using namespace em34xx::tn5250::telnet;
using em34xx::test::FakeStation;
using em34xx::test::FakeStationOptions;
using tn5250_test::Bytes;

namespace {

// The server's side of one connection. The client's I/O thread calls the
// Transport methods; the test thread reads what the client sent.
struct ServerScript {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Bytes> to_client;      // one chunk per recv()
    Bytes from_client;
    bool connected = false;
    bool closed = false;              // the server hangs up
    std::string host;
    uint16_t port = 0;

    void push(Bytes b) {
        std::lock_guard<std::mutex> g(mu);
        to_client.push_back(std::move(b));
        cv.notify_all();
    }
    Bytes sent() {
        std::lock_guard<std::mutex> g(mu);
        return from_client;
    }
};

class ScriptTransport : public em34xx::tn5250::Transport {
public:
    explicit ScriptTransport(std::shared_ptr<ServerScript> s) : s_(std::move(s)) {}
    bool connect(const std::string& host, uint16_t port, std::string&) override {
        std::lock_guard<std::mutex> g(s_->mu);
        s_->connected = true;
        s_->host = host;
        s_->port = port;
        return true;
    }
    int recv(uint8_t* buf, size_t len, int timeout_ms) override {
        std::unique_lock<std::mutex> g(s_->mu);
        s_->cv.wait_for(g, std::chrono::milliseconds(timeout_ms),
                        [&] { return !s_->to_client.empty() || s_->closed; });
        if (s_->to_client.empty()) return s_->closed ? -1 : 0;
        Bytes& front = s_->to_client.front();
        const size_t n = std::min(len, front.size());
        std::copy(front.begin(), front.begin() + std::ptrdiff_t(n), buf);
        front.erase(front.begin(), front.begin() + std::ptrdiff_t(n));
        if (front.empty()) s_->to_client.pop_front();
        return int(n);
    }
    bool send(const uint8_t* buf, size_t len) override {
        std::lock_guard<std::mutex> g(s_->mu);
        s_->from_client.insert(s_->from_client.end(), buf, buf + len);
        return true;
    }
    void close() override {}
    std::string describe() const override { return "script"; }

private:
    std::shared_ptr<ServerScript> s_;
};

// The negotiation an IBM i server runs (RFC 1205 order), in one chunk.
Bytes negotiation() {
    return {IAC, DO, OPT_TTYPE, IAC, SB, OPT_TTYPE, TT_SEND, IAC, SE, IAC, DO, OPT_EOR, IAC, WILL, OPT_EOR,
            IAC, DO, OPT_BINARY, IAC, WILL, OPT_BINARY};
}

Bytes framed_record(const Bytes& data, uint8_t opcode = em34xx::tn5250::kOpPutGet) {
    em34xx::tn5250::Record r;
    r.opcode = opcode;
    r.data = data;
    return em34xx::tn5250::TelnetCodec::frame(encode_record(r));
}

// Records in the client's byte stream (telnet commands skipped).
std::vector<em34xx::tn5250::Record> client_records(const Bytes& in) {
    std::vector<em34xx::tn5250::Record> out;
    Bytes cur;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] != IAC) { cur.push_back(in[i]); continue; }
        if (i + 1 >= in.size()) break;
        const uint8_t c = in[i + 1];
        if (c == IAC) { cur.push_back(IAC); ++i; }
        else if (c == EOR) {
            em34xx::tn5250::Record r;
            std::string why;
            if (em34xx::tn5250::decode_record(cur, r, why)) out.push_back(r);
            cur.clear();
            ++i;
        } else if (c == SB) {
            while (i + 1 < in.size() && !(in[i] == IAC && in[i + 1] == SE)) ++i;
            ++i;
        } else if (c == WILL || c == WONT || c == DO || c == DONT) i += 2;
        else ++i;
    }
    return out;
}

bool contains(const Bytes& hay, const std::string& needle) {
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end();
}

// The cable, as in test_wsc.cpp, around any TwinaxHost. Waits also pass
// wall time, because the client's I/O thread runs on its own.
struct Bench {
    TwinaxHost& host;
    std::map<uint8_t, FakeStation*> cable;
    std::map<uint8_t, unsigned> sent;   // messages per station address
    uint64_t t = 0;

    explicit Bench(TwinaxHost& h) : host(h) {}
    void plug(FakeStation& s) { cable[s.address()] = &s; }
    void step() {
        host.advance(t);
        TwinaxMessage m;
        if (!host.to_station(m)) { t += 100'000; return; }
        ++sent[twinax_message_station(m)];
        t += 16'000 * m.size();
        TwinaxMessage r;
        auto it = cable.find(twinax_message_station(m));
        if (it != cable.end() && it->second->receive(m, r)) {
            t += 45'000 + 16'000 * r.size();
            host.advance(t);
            host.from_station(r);
        } else {
            t += 60'000;
            host.advance(t);
            host.no_response();
        }
    }
    bool run_until(const std::function<bool()>& done, int wall_ms = 5000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(wall_ms);
        while (std::chrono::steady_clock::now() < end) {
            for (int k = 0; k < 200; ++k) {
                if (done()) return true;
                step();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return done();
    }
    void run_for_emulated(uint64_t ns) {
        const uint64_t end = t + ns;
        while (t < end) step();
    }
};

std::string row_text(const FakeStation& f, unsigned row, unsigned col, size_t n) {
    const auto* cp = em34xx::tn5250::CodePage::find("37");
    std::string s;
    for (size_t k = 0; k < n; ++k) {
        const uint8_t b = f.cell(row, unsigned(col + k));
        s += b >= 0x40 ? char(cp->to_unicode(b)) : ' ';
    }
    return s;
}

} // namespace

TEST_CASE("tn5250 bridge: sign-on screen to the station, keystrokes back to the host") {
    TwinaxConfig tw;
    tw.host = TwinaxHostKind::Tn5250;
    tw.stations = {StationConfig{0, StationType::Display, "EM34XXA", ""},
                   StationConfig{1, StationType::Printer, "EM34XXP", ""}};
    Tn5250Config tn;
    tn.host = "ibmi.example";
    tn.port = 2323;

    auto server = std::make_shared<ServerScript>();
    int connects = 0;
    std::vector<std::string> station_devnames;
    BridgeOptions opt;
    opt.model = "3477";
    opt.display = DisplaySubtype::Green;
    opt.reconnect_min_ms = 400;
    opt.transport = [&](const StationConfig& st, std::string&) -> std::unique_ptr<em34xx::tn5250::Transport> {
        ++connects;
        station_devnames.push_back(st.device_name);
        return std::make_unique<ScriptTransport>(server);
    };
    std::string err;
    auto host = make_tn5250_bridge(tw, tn, opt, err);
    REQUIRE(host);
    auto& bridge = static_cast<Tn5250Bridge&>(*host);
    CHECK(std::string(bridge.kind()) == "tn5250");
    REQUIRE(bridge.display(0));
    REQUIRE(bridge.printer(1));
    CHECK(bridge.display(1) == nullptr);
    CHECK(bridge.display(0)->config().terminal_type == "IBM-3477-FG");
    CHECK(bridge.display(0)->config().device_name == "EM34XXA");

    Bench b(bridge);
    FakeStation f(0);
    FakeStationOptions popt;
    popt.type = StationType::Printer;
    FakeStation p(1, popt);
    b.plug(f);
    b.plug(p);

    // The session connects at once, but until the host has negotiated the
    // cable is unplugged: nothing is sent to either station.
    REQUIRE(b.run_until([&] { return connects == 1; }));
    b.run_for_emulated(2'000'000'000);
    CHECK(b.sent[0] == 0);
    CHECK(b.sent[1] == 0);
    CHECK(bridge.describe().find("station 0 display: cable unplugged") != std::string::npos);

    // Negotiation done: the cable is plugged, the station is discovered and
    // goes on line, and the sign-on panel reaches the station's buffer.
    server->push(negotiation());
    server->push(framed_record(tn5250_test::signon_stream()));
    REQUIRE(b.run_until([&] { return row_text(f, 0, 35, 7) == "Sign On"; }));
    CHECK(connects == 1);
    CHECK(bridge.describe().find("station 0 display: cable plugged") != std::string::npos);
    REQUIRE(station_devnames.size() == 1);
    CHECK(station_devnames[0] == "EM34XXA");
    CHECK(server->host == "ibmi.example");
    CHECK(server->port == 2323);
    CHECK(row_text(f, 5, 16, 4) == "User");
    CHECK(f.cell(5, 51) == 0x24);   // the user field's attribute
    // Cursor at the insert-cursor position once the paint has finished.
    CHECK(b.run_until([&] { return f.cursor() == 5u * 80 + 52; }));
    const Bytes neg = server->sent();
    CHECK(contains(neg, "IBM-3477-FG"));

    // The printer is identified and polled; its placeholder takes no data.
    REQUIRE(b.run_until([&] { return bridge.printer(1)->ready(); }));
    CHECK(p.printed().empty());
    CHECK(bridge.describe().find("session not implemented (placeholder), on line") != std::string::npos);

    // Type 'a' (monocase field: A) and Enter on the station's keyboard.
    REQUIRE(b.run_until([&] { return (f.indicators() & 0x02) == 0; }));   // keyboard unlocked
    f.press(0x1C);
    f.press(0x58);
    std::vector<em34xx::tn5250::Record> recs;
    REQUIRE(b.run_until([&] {
        recs = client_records(server->sent());
        return !recs.empty();
    }));
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].opcode == em34xx::tn5250::kOpPutGet);
    // Cursor 06/36, AID Enter, then SBA 06/35 and the field's data.
    CHECK(recs[0].data == Bytes{0x06, 0x36, 0xF1, 0x11, 0x06, 0x35, 0xC1});
    // The typed character was painted on the station as well.
    CHECK(f.cell(5, 52) == 0xC1);

    // Device lost and back: the same session, the screen repainted.
    f.set_powered(false);
    REQUIRE(b.run_until([&] { return !bridge.display(0)->online(); }));
    f.set_powered(true);
    REQUIRE(b.run_until([&] { return bridge.display(0)->online() && row_text(f, 0, 35, 7) == "Sign On"; }));
    CHECK(bridge.display(0)->sessions_started() == 1);
    CHECK(connects == 1);

    // The host hangs up: the cable is pulled. Neither station is polled
    // any more, so the terminal sees no controller.
    Station& link = *bridge.scheduler().station(0);
    CHECK(link.counters().discoveries == 2);   // power-on, then the power cycle above
    {
        std::lock_guard<std::mutex> g(server->mu);
        server->closed = true;
        server->cv.notify_all();
    }
    REQUIRE(b.run_until([&] { return !link.plugged(); }));
    CHECK(link.state() == StationState::Absent);
    CHECK_FALSE(bridge.display(0)->online());
    b.run_for_emulated(5'000'000);   // a message already on the line completes
    const unsigned before0 = b.sent[0], before1 = b.sent[1];
    b.run_for_emulated(3'000'000'000);
    CHECK(b.sent[0] == before0);
    CHECK(b.sent[1] == before1);
    CHECK(bridge.describe().find("cable unplugged") != std::string::npos);
    // Without polls the terminal resets itself (its ~2.4 s timeout).
    f.power_on();
    p.power_on();

    // The bridge reconnects after its back-off; once negotiated the cable is
    // plugged again and the full discovery runs from scratch.
    {
        std::lock_guard<std::mutex> g(server->mu);
        server->closed = false;
        server->to_client.push_back(negotiation());
        server->to_client.push_back(framed_record(tn5250_test::signon_stream()));
    }
    REQUIRE(b.run_until([&] { return connects == 2; }, 8000));
    REQUIRE(b.run_until([&] { return link.plugged() && link.state() == StationState::Online; }));
    CHECK(link.counters().discoveries == 3);
    CHECK(b.sent[0] > before0);
    REQUIRE(b.run_until([&] { return row_text(f, 0, 35, 7) == "Sign On"; }));
    REQUIRE(b.run_until([&] { return bridge.printer(1)->ready(); }));
    CHECK(bridge.display(0)->sessions_started() == 2);

    // Monitor `disconnect 0`: the cable is pulled and stays pulled.
    REQUIRE(bridge.hold(0));
    b.run_for_emulated(5'000'000);
    CHECK_FALSE(link.plugged());
    const unsigned held0 = b.sent[0];
    b.run_until([] { return false; }, 600);   // past the reconnect back-off
    CHECK(b.sent[0] == held0);
    CHECK(connects == 2);
    CHECK(bridge.describe().find("(disconnected)") != std::string::npos);
}

TEST_CASE("tn5250 bridge: a click on a 3486/3487 moves the text cursor, a double click is Enter") {
    TwinaxConfig tw;
    tw.host = TwinaxHostKind::Tn5250;
    tw.stations = {StationConfig{0, StationType::Display, "EM34XXA", ""}};
    Tn5250Config tn;
    tn.host = "ibmi.example";
    auto server = std::make_shared<ServerScript>();
    BridgeOptions opt;
    opt.model = "3487";
    opt.display = DisplaySubtype::Green;
    opt.transport = [&](const StationConfig&, std::string&) -> std::unique_ptr<em34xx::tn5250::Transport> {
        return std::make_unique<ScriptTransport>(server);
    };
    std::string err;
    auto host = make_tn5250_bridge(tw, tn, opt, err);
    REQUIRE(host);
    auto& bridge = static_cast<Tn5250Bridge&>(*host);

    Bench b(bridge);
    FakeStationOptions o;
    o.is_348x = true;
    o.pointer = true;
    FakeStation f(0, o);
    b.plug(f);
    server->push(negotiation());
    server->push(framed_record(tn5250_test::signon_stream()));
    REQUIRE(b.run_until([&] { return row_text(f, 0, 35, 7) == "Sign On" && f.cursor() == 5u * 80 + 52; }));
    REQUIRE(b.run_until([&] { return f.pointer_reporting() && (f.indicators() & 0x02) == 0; }));

    // Left button pressed in the user field, three columns in: the whole
    // chain from the station's record to the engine moves the text cursor.
    f.pointer_event(1, 0, 0, 5u * 80 + 55);
    REQUIRE(b.run_until([&] { return f.cursor() == 5u * 80 + 55; }));
    f.pointer_event(2, 0, 0, 5u * 80 + 55);
    CHECK(client_records(server->sent()).empty());

    // Pressed again as a double click: Enter, with the cursor where it is.
    f.pointer_event(3, 0, 0, 5u * 80 + 55);
    std::vector<em34xx::tn5250::Record> recs;
    REQUIRE(b.run_until([&] {
        recs = client_records(server->sent());
        return !recs.empty();
    }));
    REQUIRE(recs.size() == 1);
    CHECK(recs[0].data == Bytes{0x06, 0x38, 0xF1});
}

TEST_CASE("tn5250 bridge: per-station session settings") {
    Tn5250Config tn;
    tn.host = "h";
    tn.device_name = "SHARED";
    tn.codepage = "273";
    tn.port = 992;
    tn.tls = true;
    tn.ca_file = "ca.pem";
    StationConfig a{0, StationType::Display, "", ""};
    StationConfig b{1, StationType::Display, "SECOND", "IBM-3179-2"};
    StationConfig c{2, StationType::Display, "", ""};
    const Tn5250Config ca = station_session_config(tn, a, true, "3477", DisplaySubtype::Color);
    CHECK(ca.device_name == "SHARED");
    CHECK(ca.terminal_type == "IBM-3477-FC");
    CHECK(ca.codepage == "273");
    // TLS settings reach every station's session unchanged.
    CHECK(ca.port == 992);
    CHECK(ca.tls);
    CHECK(ca.ca_file == "ca.pem");
    const Tn5250Config cb = station_session_config(tn, b, false, "3477", DisplaySubtype::Color);
    CHECK(cb.device_name == "SECOND");
    CHECK(cb.terminal_type == "IBM-3179-2");
    const Tn5250Config cc = station_session_config(tn, c, false, "3477", DisplaySubtype::Green);
    CHECK(cc.device_name.empty());
    CHECK(cc.terminal_type == "IBM-3477-FG");
    // A station may lower the TLS minimum for its own session.
    CHECK(cb.tls_min_version == TlsVersion::Tls1_2);
    c.tls_min_version = "1.0";
    CHECK(station_session_config(tn, c, false, "3477", DisplaySubtype::Green).tls_min_version == TlsVersion::Tls1_0);
    // A session to another system: its own host, port and pinned certificate.
    tn.tls_verify = TlsVerify::None;
    StationConfig d{1, StationType::Display, "", ""};
    d.host = "other";
    d.port = 9920;
    d.tls_cert_sha256.clear();
    for (int i = 0; i < 32; ++i) d.tls_cert_sha256 += i ? ":AB" : "AB";
    const Tn5250Config cd = station_session_config(tn, d, false, "3477", DisplaySubtype::Color);
    CHECK(cd.host == "other");
    CHECK(cd.port == 9920);
    CHECK(cd.tls_cert_sha256 == d.tls_cert_sha256);
    CHECK(cd.tls_verify == TlsVerify::System);   // the pin decides alone
    CHECK(cd.validate().empty());
    CHECK(ca.host == "h");
    TwinaxConfig tw;
    tw.stations = {a, d};
    CHECK(tw.validate().empty());
    tw.stations[1].tls_cert_sha256 = "nope";
    CHECK(tw.validate().find("tls_cert_sha256") != std::string::npos);
}

TEST_CASE("tn5250 bridge: make_twinax_host uses the bridge factory") {
    TwinaxConfig tw;
    tw.host = TwinaxHostKind::Tn5250;
    Tn5250Config tn;
    tn.host = "ibmi.example";
    std::string err;
    bool called = false;
    auto h = make_twinax_host(tw, tn, err, [&](const TwinaxConfig& t, const Tn5250Config& c, std::string& e) {
        called = true;
        BridgeOptions o;
        o.transport = [](const StationConfig&, std::string& why) -> std::unique_ptr<em34xx::tn5250::Transport> {
            why = "no network in this test";
            return nullptr;
        };
        o.reconnect = false;
        return make_tn5250_bridge(t, c, o, e);
    });
    REQUIRE(h);
    CHECK(called);
    CHECK(dynamic_cast<Tn5250Bridge*>(h.get()) != nullptr);
    CHECK(h->describe().find("station 0 display: cable unplugged") != std::string::npos);
}

TEST_CASE("tn5250 bridge: typing on the 122-key keyboard echoes and reaches the host") {
    TwinaxConfig tw;
    tw.host = TwinaxHostKind::Tn5250;
    Tn5250Config tn;
    tn.host = "ibmi.example";
    auto server = std::make_shared<ServerScript>();
    server->push(negotiation());
    server->push(framed_record(tn5250_test::signon_stream()));
    BridgeOptions opt;
    opt.transport = [&](const StationConfig&, std::string&) -> std::unique_ptr<em34xx::tn5250::Transport> {
        return std::make_unique<ScriptTransport>(server);
    };
    std::string err;
    auto host = make_tn5250_bridge(tw, tn, opt, err);
    REQUIRE(host);
    Bench b(*host);
    FakeStationOptions ko;
    ko.keyboard_id = 0x02;   // 122-key typewriter keyboard: 3180-2 style cable codes
    FakeStation f(0, ko);
    b.plug(f);
    REQUIRE(b.run_until([&] { return row_text(f, 0, 35, 7) == "Sign On"; }));
    REQUIRE(b.run_until([&] { return (f.indicators() & 0x02) == 0 && f.cursor() == 5u * 80 + 52; }));
    f.press(0x11);   // a
    f.press(0x13);   // d
    // The characters are echoed on the station (monocase field: A D).
    REQUIRE(b.run_until([&] { return f.cell(5, 52) == 0xC1 && f.cell(5, 53) == 0xC4; }));
    f.press(0x68);   // Enter
    std::vector<em34xx::tn5250::Record> recs;
    REQUIRE(b.run_until([&] {
        recs = client_records(server->sent());
        return !recs.empty();
    }));
    CHECK(recs[0].data == Bytes{0x06, 0x37, 0xF1, 0x11, 0x06, 0x35, 0xC1, 0xC4});

    // The password field (non-display): what is typed reaches the host but
    // never the station's buffer.
    server->push(framed_record(tn5250_test::signon_stream()));   // the panel again
    REQUIRE(b.run_until([&] { return (f.indicators() & 0x02) == 0; }));
    f.press(0x20);   // Field Advance -> password field
    f.press(0x02);   // x
    f.press(0x03);   // c
    REQUIRE(b.run_until([&] { return f.cursor() == 6u * 80 + 54; }));
    for (unsigned c = 52; c < 62; ++c) CHECK(f.cell(6, c) == 0x00);
    f.press(0x68);
    REQUIRE(b.run_until([&] {
        recs = client_records(server->sent());
        return recs.size() >= 2;
    }));
    const Bytes& d = recs[1].data;
    const Bytes pw{0x11, 0x07, 0x35, 0xA7, 0x83};
    CHECK(std::search(d.begin(), d.end(), pw.begin(), pw.end()) != d.end());
}

TEST_CASE("tn5250 bridge: the first paint of a long run arrives whole by Activate Write") {
    TwinaxConfig tw;
    tw.host = TwinaxHostKind::Tn5250;
    Tn5250Config tn;
    tn.host = "ibmi.example";
    auto server = std::make_shared<ServerScript>();
    server->push(negotiation());
    // A full row of '=' (longer than any queue load) and text after it.
    Bytes rule(80, 0x7E);
    server->push(framed_record(tn5250_test::cat({{0x04, 0x40},
                                                  {0x04, 0x11, 0x00, 0x08},
                                                  {0x11, 0x0A, 0x01},
                                                  rule,
                                                  {0x11, 0x0B, 0x05},
                                                  tn5250_test::eb("Welcome")})));
    BridgeOptions opt;
    opt.model = "3477";
    opt.transport = [&](const StationConfig&, std::string&) -> std::unique_ptr<em34xx::tn5250::Transport> {
        return std::make_unique<ScriptTransport>(server);
    };
    std::string err;
    auto host = make_tn5250_bridge(tw, tn, opt, err);
    REQUIRE(host);
    Bench b(*host);
    FakeStation f(0);
    b.plug(f);
    REQUIRE(b.run_until([&] { return row_text(f, 10, 4, 7) == "Welcome"; }));
    for (unsigned c = 0; c < 80; ++c) CHECK(f.cell(9, c) == 0x7E);
    // Painted with Write Data + Activate Write, once: the device was never
    // lost and rediscovered.
    CHECK(f.activate_writes() >= 1);
    CHECK(f.device_id_reads() == 1);
}

TEST_CASE("tn5250 bridge: session status for a status bar") {
    TwinaxConfig tw;
    tw.host = TwinaxHostKind::Tn5250;
    tw.stations = {StationConfig{0, StationType::Display, "", ""}, StationConfig{1, StationType::Printer, "", ""}};
    Tn5250Config tn;
    tn.host = "status.example";
    tn.port = 2323;
    wsc::BridgeOptions opt;
    opt.reconnect = false;
    opt.transport = [](const StationConfig&, std::string& error) -> std::unique_ptr<tn5250::Transport> {
        error = "no network in this test";
        return nullptr;
    };
    std::string err;
    auto bridge = wsc::make_tn5250_bridge(tw, tn, opt, err);
    REQUIRE(bridge);
    std::vector<TwinaxSessionStatus> st;
    for (int i = 0; i < 200; ++i) {
        st = bridge->session_status();
        REQUIRE(st.size() == 2);
        if (st[0].state == "failed") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(st[0].address == 0);
    CHECK_FALSE(st[0].printer);
    CHECK(st[0].peer == "status.example:2323");
    CHECK(st[0].state == "failed");
    CHECK(st[0].error.find("no network") != std::string::npos);
    CHECK_FALSE(st[0].connected);
    CHECK(st[1].printer);
    REQUIRE(bridge->disconnect_station(0));
    CHECK(bridge->session_status()[0].state == "disconnected");
    CHECK(bridge->session_status()[0].held);
}
