// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/wsc/station.hpp"

#include "host/wsc/device_commands.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

namespace em34xx {
namespace wsc {

const char* station_state_name(StationState s) {
    switch (s) {
    case StationState::Absent: return "absent";
    case StationState::PowerOn: return "power-on";
    case StationState::Identify: return "identify";
    case StationState::Init: return "init";
    case StationState::Online: return "online";
    case StationState::Recover: return "recover";
    }
    return "?";
}

bool StationIdentity::is_3477() const {
    return vpd.size() >= 4 && vpd[0] == 0xF3 && vpd[1] == 0xF4 && vpd[2] == 0xF7 && vpd[3] == 0xF7;
}

// Bytes 51 and 52 of the 3477-format VPD: 00 00 on a 3477, 1F 9F (DF with
// a pointing device) on a 3486/3487 [obs; what the bits mean is unknown].
bool StationIdentity::offers_native_vpd() const { return is_3477() && vpd.size() > 52 && vpd[52] != 0; }

bool StationIdentity::is_348x() const {
    return native_vpd.size() >= 3 && native_vpd[0] == 0xF3 && native_vpd[1] == 0xF4 && native_vpd[2] == 0xF8;
}

bool StationIdentity::has_pointer() const {
    return is_348x() && native_vpd.size() > 52 && (native_vpd[52] & 0x40) != 0;
}

bool StationIdentity::colour() const { return model_id == 0x50; }

// The 3477 supports 27x132 on every model [doc PAI 16.0]; the 3197 only on
// model D, which answers model ID 24h [doc PAI 14.0].
bool StationIdentity::wide_capable() const { return is_3477() || model_id == 0x24; }

std::string StationIdentity::describe() const {
    if (type == StationType::Printer)
        return format("printer, ID %02X (%s)", device_id, printer_type_name(device_id));
    std::string out = format("display, ID %02X, model %02X%s", device_id, model_id, is_3477() ? " (3477)" : "");
    if (has_keyboard_id)
        out += format(", keyboard %02X (%s)", keyboard_id, enhanced_keyboard() ? "enhanced" : "typewriter");
    if (is_348x()) out += has_pointer() ? ", 348x with a pointing device" : ", 348x";
    out += wide_capable() ? ", 27x132" : ", 24x80";
    return out;
}

bool device_id_is_printer(uint8_t id) { return ((id >> 4) & 3) == 2; }

const char* printer_type_name(uint8_t id) {
    if (!device_id_is_printer(id)) return "?";
    switch (id & 0x0F) {
    case 0x0: return "5256";
    case 0xC: return "4214-2";
    case 0xD: return "5219";
    default: return "?";
    }
}

Station::Station(const StationConfig& cfg, const WscTiming& timing) : cfg_(cfg), timing_(timing) {
    id_.address = uint8_t(cfg.address);
    id_.type = cfg.type;
}

void Station::enter(StationState s, uint64_t) {
    if (s != state_)
        LOG_DEBUG("wsc", "station %u: %s -> %s", cfg_.address, station_state_name(state_), station_state_name(s));
    state_ = s;
}

void Station::set_plugged(bool on, uint64_t now) {
    if (on == plugged_) return;
    plugged_ = on;
    LOG_INFO("wsc", "station %u: cable %s", cfg_.address, on ? "plugged in" : "unplugged");
    if (on) {
        ready_at_ = now;   // probe at once
        return;
    }
    if (announced_) {
        if (display_) display_->device_lost();
        if (printer_) printer_->device_lost();
        announced_ = false;
    }
    enter(StationState::Absent, now);
    ops_.clear();
    expect_ = Expect::Nothing;
    waiting_ = false;
    misses_ = 0;
    reset_lpe_ = false;
    level_ = -1;
    printer_status_ = -1;
    keymap_.reset();
    type_mismatch_ = false;
    poll_soon_ = false;
    mirror_ = DeviceMirror();
    flushed_generation_ = ~uint64_t(0);
}

bool Station::due(uint64_t now) const {
    if (!plugged_) return false;
    if (now >= ready_at_) return true;
    if (!ops_.empty() || state_ != StationState::Online || type_mismatch_) return false;
    // New screen content does not wait for the idle poll.
    return display_ && display_->screen_generation() != flushed_generation_;
}

void Station::next_message(uint64_t now, TwinaxMessage& out) {
    ++counters_.messages;
    // A button event waits for its read for two polls only [obs], so the
    // read goes ahead of any other work at the next step boundary.
    if (pointer_pending_ && state_ == StationState::Online && (ops_.empty() || ops_.front().kind == OpKind::Load))
        queue_pointer_read();
    if (ops_.empty() && state_ == StationState::Online) plan_online_work(now);
    if (ops_.empty()) {
        out = make_poll(address(), reset_lpe_);
        expect_ = Expect::PollStatus;
        ++counters_.polls;
        return;
    }
    Op& op = ops_.front();
    switch (op.kind) {
    case OpKind::Load:
        out = make_message(op.bytes, address());
        expect_ = Expect::Nothing;
        break;
    case OpKind::WaitReady:
        if (!waiting_) {
            waiting_ = true;
            wait_started_ = now;
        }
        out = make_poll(address(), reset_lpe_);
        expect_ = Expect::PollStatus;
        ++counters_.polls;
        break;
    case OpKind::ActivateRead:
        out = make_message({cmd::ActivateRead}, address());
        expect_ = Expect::ReadData;
        break;
    case OpKind::ActivateWrite: {
        std::vector<uint8_t> b;
        b.reserve(op.bytes.size() + 1);
        b.push_back(cmd::ActivateWrite);
        b.insert(b.end(), op.bytes.begin(), op.bytes.end());
        out = make_message(b, address());
        expect_ = Expect::Nothing;
        break;
    }
    case OpKind::Call:   // never left at the front (op_done runs them)
        out = make_poll(address(), reset_lpe_);
        expect_ = Expect::PollStatus;
        break;
    }
    if (op.kind != OpKind::WaitReady)
        LOG_TRACE("wsc", "station %u: send %s", cfg_.address, format_twinax_message(out).c_str());
}

void Station::op_done(uint64_t now) {
    uint64_t pause = 0;
    if (!ops_.empty()) {
        pause = ops_.front().pause_ns;
        ops_.pop_front();
    }
    waiting_ = false;
    ready_at_ = now + pause;
    while (!ops_.empty() && ops_.front().kind == OpKind::Call) {
        std::function<void()> fn = std::move(ops_.front().call);
        ops_.pop_front();
        fn();
    }
    if (ops_.empty() && state_ == StationState::Online && !poll_soon())
        ready_at_ = now + timing_.idle_poll_ns;
}

void Station::on_response(const TwinaxMessage& msg, uint64_t now) {
    if (!plugged_) return;   // the answer to a message sent before the cable was pulled
    misses_ = 0;
    const Expect expect = expect_;
    expect_ = Expect::Nothing;
    if (expect == Expect::ReadData || (!msg.empty() && (msg[0].data & (status::Busy | status::OutstandingStatus))))
        LOG_TRACE("wsc", "station %u: recv %s", cfg_.address, format_twinax_message(msg).c_str());
    if (expect == Expect::ReadData) {
        std::vector<uint8_t> data;
        for (const TwinaxFrame& f : msg) data.push_back(f.data);
        if (!ops_.empty() && ops_.front().kind == OpKind::ActivateRead) {
            auto cb = ops_.front().on_data;
            if (cb) cb(data);
        }
        op_done(now);
        return;
    }
    if (expect == Expect::Nothing) {   // a station does not answer loads; carry on
        op_done(now);
        return;
    }
    if (msg.empty()) return;
    const uint8_t status = msg[0].data;
    const bool two = msg.size() >= 2;
    const uint8_t byte2 = two ? msg[1].data : 0;
    if (!handle_status(status, two, byte2, now)) return;

    if (!ops_.empty() && ops_.front().kind == OpKind::WaitReady) {
        if (status & status::Busy) {
            if (now - wait_started_ > timing_.busy_timeout_ns) {
                lose(now, "busy timeout");
                return;
            }
            ready_at_ = now + timing_.busy_repoll_ns;
            return;
        }
        op_done(now);
        return;
    }
    // An idle poll or an absent-address probe.
    ready_at_ = poll_soon() ? now : now + timing_.idle_poll_ns;
}

void Station::on_no_response(uint64_t now) {
    if (!plugged_) return;
    const Expect expect = expect_;
    expect_ = Expect::Nothing;
    if (expect == Expect::Nothing) {   // a load or Activate Write went out
        op_done(now);
        return;
    }
    ++counters_.no_responses;
    if (state_ == StationState::Absent) {
        ready_at_ = now + timing_.absent_probe_ns;
        return;
    }
    if (++misses_ >= timing_.no_response_limit) {
        lose(now, "no response");
        return;
    }
    ready_at_ = now + timing_.busy_repoll_ns;   // retry the same operation
}

void Station::lose(uint64_t now, const char* why) {
    LOG_INFO("wsc", "station %u: lost (%s)", cfg_.address, why);
    if (announced_) {
        if (display_) display_->device_lost();
        if (printer_) printer_->device_lost();
        announced_ = false;
    }
    enter(StationState::Absent, now);
    ops_.clear();
    waiting_ = false;
    misses_ = 0;
    reset_lpe_ = false;
    mirror_ = DeviceMirror();
    ready_at_ = now + timing_.absent_probe_ns;
}

bool Station::handle_status(uint8_t st, bool two_frames, uint8_t byte2, uint64_t now) {
    if (state_ == StationState::Absent) {
        begin_discovery(st, two_frames, now);
        return false;
    }
    if (st & status::LineParityError) {
        ++counters_.parity_errors;
        reset_lpe_ = true;
        recover(st, now);
        return false;
    }
    reset_lpe_ = false;
    const uint8_t exc = status::exception(st);
    if (exc == status::ExcPowerOn) {
        if (state_ == StationState::PowerOn && (st & status::Busy)) return true;   // Set Mode still pending
        if (state_ == StationState::PowerOn) {
            // Set Mode did not take: send it again a little later.
            begin_discovery(st, two_frames, now);
            ready_at_ = now + timing_.busy_repoll_ns;
            return false;
        }
        LOG_INFO("wsc", "station %u: power-on status, rediscovering", cfg_.address);
        if (announced_) {
            if (display_) display_->device_lost();
            if (printer_) printer_->device_lost();
            announced_ = false;
        }
        begin_discovery(st, two_frames, now);
        return false;
    }
    if (exc != status::ExcNone) {
        ++counters_.exceptions;
        // An optional identification read the device refused: reset the
        // exception and skip the rest of that read.
        if (!ops_.empty() && ops_.front().group != 0 && state_ == StationState::Identify) {
            const int group = ops_.front().group;
            while (!ops_.empty() && ops_.front().group == group) ops_.pop_front();
            Op w;
            w.kind = OpKind::WaitReady;
            ops_.push_front(w);
            Op l;
            l.kind = OpKind::Load;
            l.bytes = QueueLoad().write_control(wcd::ResetException).end_of_queue().bytes();
            ops_.push_front(l);
            waiting_ = false;
            ready_at_ = now;
            return false;
        }
        recover(st, now);
        return false;
    }
    if (two_frames) handle_byte2(uint8_t(st & status::Level), byte2);
    if ((st & status::OutstandingStatus) && pointer_enabled_ && state_ == StationState::Online)
        pointer_pending_ = true;
    return true;
}

// The pointer record, read as soon as outstanding status shows: the read
// command and then Activate Read with no poll in between, because every
// poll moves the terminal's offer on [obs]. Goes to the front of the work.
void Station::queue_pointer_read() {
    pointer_pending_ = false;
    Op r;
    r.kind = OpKind::ActivateRead;
    r.on_data = [this](const std::vector<uint8_t>& d) { handle_pointer_record(d); };
    ops_.push_front(r);
    Op l;
    l.kind = OpKind::Load;
    l.bytes = QueueLoad().read_record(8).bytes();
    l.pause_ns = timing_.pointer_read_ns;
    ops_.push_front(l);
}

// 8 bytes [obs]: 01 = pointer event; an event code per button (left, right,
// middle: 1 pressed, 2 released, 3 pressed again as a double click); the
// buttons now down; the pointer's buffer address, high byte first.
void Station::handle_pointer_record(const std::vector<uint8_t>& d) {
    poll_soon_ = true;   // more events may be queued behind this one
    if (d.size() < 7 || d[0] != 0x01) return;
    const unsigned cols = mirror_.cols ? mirror_.cols : 80;
    const unsigned pos = unsigned(d[5]) << 8 | d[6];
    static const PointerAction::Button buttons[3] = {PointerAction::Button::Left, PointerAction::Button::Right,
                                                     PointerAction::Button::Middle};
    for (int b = 0; b < 3; ++b) {
        const uint8_t code = uint8_t(d[size_t(1 + b)] & 0x0F);
        if (code < 1 || code > 3) continue;
        PointerAction a;
        a.button = buttons[b];
        a.kind = code == 1 ? PointerAction::Kind::Press
                 : code == 2 ? PointerAction::Kind::Release : PointerAction::Kind::DoubleClick;
        a.shift = keymap_.shift();   // the terminal does not report it; the keyboard does
        a.row = pos / cols;
        a.col = pos % cols;
        ++counters_.pointer_events;
        LOG_DEBUG("wsc", "station %u: pointer %s %s at %u,%u%s", cfg_.address,
                  b == 0 ? "left" : b == 1 ? "right" : "middle",
                  code == 1 ? "pressed" : code == 2 ? "released" : "double click", a.row + 1, a.col + 1,
                  a.shift ? " (shift)" : "");
        if (display_) display_->pointer_action(a);
    }
}

void Station::handle_byte2(uint8_t level, uint8_t byte2) {
    if (id_.type == StationType::Printer && state_ == StationState::Online) {
        if (int(byte2) != printer_status_) {
            printer_status_ = byte2;
            if (printer_) printer_->printer_status(byte2);
        }
        return;
    }
    // A new keystroke only when the response level changed [doc PAI p.42].
    const bool fresh = level_ < 0 || int(level) != level_;
    level_ = level;
    if (!fresh || byte2 == 0) return;
    if (state_ != StationState::Online) return;   // keys before Init are dropped
    ++counters_.keys;
    poll_soon_ = true;   // the terminal may hold more keys: drain them quickly
    OperatorAction a;
    const bool acted = keymap_.translate(byte2, a);
    if (!acted) {
        LOG_DEBUG("wsc", "station %u: scan code %02X (shift state)", cfg_.address, byte2);
    } else {
        using K = OperatorAction::Kind;
        LOG_DEBUG("wsc", "station %u: scan code %02X -> %s %02X%s%s%s", cfg_.address, byte2,
                  operator_action_name(a.kind), a.kind == K::Aid ? a.aid : a.ebcdic,
                  a.kind == K::Function ? " " : "", a.kind == K::Function ? key_function_name(a.function) : "",
                  a.shift ? " (shift)" : "");
    }
    if (acted && display_) display_->operator_action(a);
}

void Station::begin_discovery(uint8_t st, bool, uint64_t now) {
    ++counters_.discoveries;
    ops_.clear();
    waiting_ = false;
    level_ = -1;
    printer_status_ = -1;
    mirror_ = DeviceMirror();
    keymap_.reset();
    type_mismatch_ = false;
    pointer_enabled_ = false;
    pointer_pending_ = false;
    const StationType configured = cfg_.type;
    id_ = StationIdentity();
    id_.address = address();
    id_.type = configured;
    ready_at_ = now;

    const uint8_t exc = status::exception(st);
    if (exc == status::ExcPowerOn) {
        enter(StationState::PowerOn, now);
        Op l;
        l.kind = OpKind::Load;
        l.bytes = QueueLoad().set_mode(0x00).bytes();   // no inter-frame fill
        ops_.push_back(l);
    } else if (exc != status::ExcNone) {
        enter(StationState::Recover, now);
        Op l;
        l.kind = OpKind::Load;
        l.bytes = QueueLoad().write_control(wcd::ResetException).end_of_queue().bytes();
        ops_.push_back(l);
    } else {
        queue_identify();
        return;
    }
    Op w;
    w.kind = OpKind::WaitReady;
    ops_.push_back(w);
    Op c;
    c.call = [this]() { queue_identify(); };
    ops_.push_back(c);
}

void Station::queue_read(std::vector<uint8_t> load, std::function<void(const std::vector<uint8_t>&)> cb,
                         int group) {
    Op l;
    l.kind = OpKind::Load;
    l.bytes = std::move(load);
    l.group = group;
    ops_.push_back(l);
    Op w;
    w.kind = OpKind::WaitReady;
    w.group = group;
    ops_.push_back(w);
    Op r;
    r.kind = OpKind::ActivateRead;
    r.on_data = std::move(cb);
    r.group = group;
    ops_.push_back(r);
}

void Station::queue_identify() {
    enter(StationState::Identify, 0);
    queue_read({cmd::ReadDeviceId}, [this](const std::vector<uint8_t>& d) {
        id_.device_id = d.empty() ? 0 : d[0];
    }, 0);
    Op c;
    c.call = [this]() {
        id_.type = device_id_is_printer(id_.device_id) ? StationType::Printer : StationType::Display;
        type_mismatch_ = id_.type != cfg_.type;
        if (type_mismatch_)
            LOG_WARN("wsc", "station %u: configured as %s but the device reports %s (ID %02X)", cfg_.address,
                     station_type_name(cfg_.type), station_type_name(id_.type), id_.device_id);
        if (id_.type == StationType::Display) {
            // Feature-format reads (8C, 88) must precede WCD byte 2 bit 40h.
            queue_read({cmd::ReadModelId}, [this](const std::vector<uint8_t>& d) {
                if (!d.empty()) id_.model_id = d[0];
            }, 1);
            queue_read({cmd::ReadKeyboardId}, [this](const std::vector<uint8_t>& d) {
                if (d.empty()) return;
                id_.has_keyboard_id = true;
                id_.keyboard_id = d[0];
            }, 2);
            queue_read({cmd::ReadModelData}, [this](const std::vector<uint8_t>& d) {
                id_.has_extended_id = d.size() >= 16;
                for (size_t i = 0; i < 16 && i < d.size(); ++i) id_.extended_id[i] = d[i];
            }, 3);
            queue_read(QueueLoad().read_info().bytes(), [this](const std::vector<uint8_t>& d) {
                id_.vpd = d;
                LOG_DEBUG("wsc", "station %u: Read Info 80: %s", cfg_.address, hex_bytes(d.data(), d.size()).c_str());
            }, 4);
            // A 3486/3487 answers Read Info C0 with its own VPD, and only
            // then accepts a third Write Control Data byte [obs]. A 3477
            // must not be asked: its firmware latches an exception that
            // Write Control Data does not reset [obs]. The two differ in
            // the ordinary VPD's feature bytes, so ask only on that.
            Op ask;
            ask.call = [this]() {
                if (!id_.offers_native_vpd()) return;
                Op r;
                r.kind = OpKind::ActivateRead;
                r.group = 5;
                r.on_data = [this](const std::vector<uint8_t>& d) {
                    id_.native_vpd = d;
                    LOG_DEBUG("wsc", "station %u: Read Info C0: %s", cfg_.address,
                              hex_bytes(d.data(), d.size()).c_str());
                };
                ops_.push_front(r);
                Op w;
                w.kind = OpKind::WaitReady;
                w.group = 5;
                ops_.push_front(w);
                Op l;
                l.kind = OpKind::Load;
                l.group = 5;
                l.bytes = QueueLoad().read_info(cmd::ReadInfoNative).bytes();
                ops_.push_front(l);
            };
            ops_.push_back(ask);
        }
        Op init;
        init.call = [this]() { queue_init(); };
        ops_.push_back(init);
    };
    ops_.push_back(c);
}

FlushOptions Station::flush_options() const {
    FlushOptions o;
    if (id_.is_3477()) {
        o.queue_bytes = kQueueBytes3477;
        o.wcd_byte2_base = wcd::B2Queue64;
        o.has_3477_indicators = true;
    }
    if (id_.has_pointer()) {
        // Button events as records read from device 101, which needs device
        // addressing [obs; whether the read needs it is a hypothesis].
        o.wcd_byte2_base |= wcd::B2DeviceAddressing;
        o.wcd_byte3 = wcd::B3PointerEvents;
    }
    return o;
}

void Station::queue_init() {
    enter(StationState::Init, 0);
    auto online = [this]() {
        enter(StationState::Online, 0);
        LOG_INFO("wsc", "station %u: online, %s", cfg_.address, id_.describe().c_str());
        flushed_generation_ = ~uint64_t(0);
        announced_ = true;
        if (type_mismatch_) return;   // keep it polled, but drive nothing
        if (id_.type == StationType::Display && display_) display_->device_ready(id_);
        if (id_.type == StationType::Printer && printer_) printer_->device_ready(id_);
    };
    if (id_.type != StationType::Display || type_mismatch_) {
        Op c;
        c.call = online;
        ops_.push_back(c);
        return;
    }
    keymap_.set_keyboard_id(id_.has_keyboard_id ? id_.keyboard_id : 0x02);
    const FlushOptions o = flush_options();
    ScreenState blank;   // 24x80, cursor home, input inhibited
    const uint8_t b1 = wcd_byte1_for(blank);
    const uint8_t b2 = wcd_byte2_for(blank, o);
    const unsigned n = blank.rows * blank.cols;

    Op l1;
    l1.kind = OpKind::Load;
    QueueLoad q1;
    q1.write_control(b1, b2, o.wcd_byte3);
    if (id_.has_pointer()) q1.select_record(cmd::RecordPointer);
    l1.bytes = q1.end_of_queue().bytes();
    ops_.push_back(l1);
    Op w;
    w.kind = OpKind::WaitReady;
    ops_.push_back(w);
    Op l2;
    l2.kind = OpKind::Load;
    l2.bytes = QueueLoad()
                   .load_address(0)
                   .load_reference(n - 1)
                   .clear()
                   .write_indicators(blank.indicators)
                   .load_cursor(0)
                   .end_of_queue()
                   .bytes();
    ops_.push_back(l2);
    ops_.push_back(w);
    Op c;
    const int b3 = o.wcd_byte3;
    c.call = [this, b1, b2, b3, blank, online]() {
        pointer_enabled_ = id_.has_pointer();
        pointer_pending_ = false;
        if (pointer_enabled_) LOG_INFO("wsc", "station %u: pointer button events enabled", cfg_.address);
        mirror_.wcd_byte3 = b3;
        mirror_.set_format(blank.rows, blank.cols);
        std::fill(mirror_.cells.begin(), mirror_.cells.end(), int16_t(0));
        mirror_.cursor = 0;
        mirror_.indicators = blank.indicators;
        mirror_.wcd_byte1 = b1;
        mirror_.wcd_byte2 = b2;
        online();
    };
    ops_.push_back(c);
}

void Station::queue_step(Step step) {
    Op l;
    l.kind = OpKind::Load;
    l.bytes = std::move(step.load);
    ops_.push_back(l);
    Op w;
    w.kind = OpKind::WaitReady;
    ops_.push_back(w);
    if (!step.payload.empty()) {
        Op a;
        a.kind = OpKind::ActivateWrite;
        a.bytes = std::move(step.payload);
        ops_.push_back(a);
        ops_.push_back(w);
    }
    Op c;
    auto apply = std::move(step.apply);
    c.call = [this, apply]() {
        if (apply) apply(mirror_);
        ++counters_.steps;
    };
    ops_.push_back(c);
}

void Station::plan_online_work(uint64_t) {
    if (type_mismatch_) return;
    if (id_.type == StationType::Display) {
        if (!display_) return;
        const uint64_t gen = display_->screen_generation();
        const bool alarm = display_->take_alarm();
        if (gen == flushed_generation_ && !alarm) return;
        flushed_generation_ = gen;
        ScreenState s;
        display_->screen_snapshot(s);
        if (s.cols == 132 && !id_.wide_capable()) {
            LOG_WARN("wsc", "station %u: 27x132 screen on a 24x80 device; not shown", cfg_.address);
            return;
        }
        for (Step& step : plan_flush(s, mirror_, alarm, flush_options())) queue_step(std::move(step));
        return;
    }
    // Printer: SCS data goes as Write Data + Activate Write [doc PAI 3.5.3].
    std::vector<uint8_t> scs;
    if (!printer_ || !printer_->next_print_data(scs) || scs.empty()) return;
    for (size_t pos = 0; pos < scs.size(); pos += timing_.printer_chunk) {
        Step step;
        step.load = {cmd::WriteData};
        const size_t len = std::min(timing_.printer_chunk, scs.size() - pos);
        step.payload.assign(scs.begin() + long(pos), scs.begin() + long(pos + len));
        queue_step(std::move(step));
    }
}

void Station::recover(uint8_t st, uint64_t now) {
    const StationState resume = state_ == StationState::Online ? StationState::Online : StationState::Identify;
    if (state_ != StationState::Recover) resume_state_ = resume;
    enter(StationState::Recover, now);
    LOG_DEBUG("wsc", "station %u: recovering from status %02X", cfg_.address, st);
    ops_.clear();
    waiting_ = false;
    ready_at_ = now;
    if (!(st & status::LineParityError)) {
        // Reset the exception; keep the screen format the device has.
        Op l;
        l.kind = OpKind::Load;
        QueueLoad q;
        if (mirror_.wcd_byte2 >= 0)
            q.write_control(uint8_t(mirror_.wcd_byte1 | wcd::ResetException), mirror_.wcd_byte2, mirror_.wcd_byte3);
        else
            q.write_control(wcd::ResetException);
        l.bytes = q.end_of_queue().bytes();
        ops_.push_back(l);
    }
    // After an LPE the next poll carries "reset LPE"; the dropped load is
    // repainted from the shadow (the 3477 also clears its exception).
    Op w;
    w.kind = OpKind::WaitReady;
    ops_.push_back(w);
    Op c;
    c.call = [this]() {
        mirror_.invalidate();
        flushed_generation_ = ~uint64_t(0);
        if (resume_state_ == StationState::Online) enter(StationState::Online, 0);
        else queue_identify();
    };
    ops_.push_back(c);
}

std::string Station::describe() const {
    std::string out = format("station %u (%s): cable %s, %s", cfg_.address, station_type_name(cfg_.type),
                             plugged_ ? "plugged" : "unplugged", station_state_name(state_));
    if (state_ == StationState::Online) out += ", " + id_.describe();
    if (type_mismatch_) out += " [type mismatch]";
    if (pointer_enabled_) out += format(", pointer events %llu", (unsigned long long)counters_.pointer_events);
    out += format("; polls %llu, keys %llu, steps %llu, exceptions %llu, LPE %llu, no-response %llu\n",
                  (unsigned long long)counters_.polls, (unsigned long long)counters_.keys,
                  (unsigned long long)counters_.steps, (unsigned long long)counters_.exceptions,
                  (unsigned long long)counters_.parity_errors, (unsigned long long)counters_.no_responses);
    return out;
}

} // namespace wsc
} // namespace em34xx
