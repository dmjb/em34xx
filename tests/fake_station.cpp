// SPDX-License-Identifier: GPL-3.0-or-later
#include "fake_station.hpp"

namespace em34xx {
namespace test {

FakeStation::FakeStation(uint8_t address, FakeStationOptions opt) : address_(address), opt_(opt) {
    power_on();
}

void FakeStation::power_on() {
    set_mode_ = false;
    exception_ = 0x0E;
    busy_ = 0;
    level_ = 0;
    keys_.clear();
    key_reg_ = 0;
    key_shown_ = false;
    armed_read_ = armed_write_ = false;
    lpe_ = false;
    wide_ = false;
    queue64_ = false;
    ps_.assign(ps_size(), 0);
    ac_ = rc_ = 0;
    cr_ = 0x4F;
    wcd1_ = wcd2_ = 0;
    indicators_ = indicators_3477_ = 0;
    native_info_ = false;
    bad_info_ = false;
    pointer_reporting_ = false;
    record_ = 1;
    pointer_events_.clear();
    pointer_offers_ = 0;
}

void FakeStation::pointer_event(uint8_t left, uint8_t right, uint8_t middle, unsigned position) {
    if (!pointer_reporting_) return;
    pointer_events_.push_back({0x01, left, right, middle, 0x00, uint8_t(position >> 8), uint8_t(position), 0x00});
}

void FakeStation::set_powered(bool on) {
    if (on && !powered_) power_on();
    powered_ = on;
}

void FakeStation::latch_exception(uint8_t code) {
    if (exception_ == 0) exception_ = code;
}

void FakeStation::press(uint8_t scan) {
    if (keys_.size() < 7) keys_.push_back(scan);
    else keys_.back() = 0xFF;   // overrun
}

void FakeStation::fail(uint8_t exc) {
    if (exception_ == 0) exception_ = exc;   // the first exception latches
    armed_read_ = armed_write_ = false;
}

void FakeStation::arm_read(std::vector<uint8_t> data) {
    read_data_ = std::move(data);
    armed_read_ = true;
}

std::vector<uint8_t> FakeStation::vpd() const {
    std::vector<uint8_t> v(64, 0);
    v[0] = 0xF3; v[1] = 0xF4; v[2] = 0xF7; v[3] = 0xF7;   // "3477"
    v[4] = 0xC6; v[5] = opt_.model_id == 0x50 ? 0xC3 : 0xC7; v[6] = 0xF0;   // FC0 / FG0
    v[50] = opt_.model_id == 0x50 ? 0xFA : 0x7A;
    if (opt_.is_348x) {
        v[51] = 0x1F;
        v[52] = opt_.pointer ? 0xDF : 0x9F;
    }
    return v;
}

bool FakeStation::receive(const TwinaxMessage& msg, TwinaxMessage& response) {
    if (!powered_ || msg.empty()) return false;
    if (drop_) {
        --drop_;
        return false;
    }
    const uint8_t c = msg[0].data;
    const uint8_t code = uint8_t(c & 0x1F);

    if (code == 0x10) {   // Poll
        if (c & 0x40) lpe_ = false;
        const bool ack = (c & 0x20) != 0;
        if (display() && set_mode_) {
            if (ack && key_shown_) {
                key_reg_ = 0;
                key_shown_ = false;
            }
            if (key_reg_ == 0 && !keys_.empty()) {
                key_reg_ = keys_.front();
                keys_.pop_front();
                level_ ^= 1;
            }
            key_shown_ = key_reg_ != 0;
        }
        const bool busy = busy_ > 0;
        if (busy) --busy_;
        // A button event shows as outstanding status on two polls and is
        // then dropped unless its record was read.
        if (!pointer_events_.empty() && pointer_offers_ == 2) {
            pointer_events_.pop_front();
            pointer_offers_ = 0;
            ++pointer_dropped_;
        }
        const bool outstanding = !pointer_events_.empty();
        if (outstanding) ++pointer_offers_;
        const uint8_t st =
            uint8_t((busy ? 0x80 : 0) | (lpe_ ? 0x40 : 0) | (outstanding ? 0x10 : 0) | exception_ | level_);
        response.clear();
        if (!set_mode_) {
            response.push_back(TwinaxFrame{st, 7});
        } else {
            response.push_back(TwinaxFrame{st, address_});
            response.push_back(TwinaxFrame{uint8_t(display() ? key_reg_ : printer_status), 7});
        }
        return true;
    }
    if (lpe_) return false;   // only polls are honoured until a reset-LPE poll

    std::vector<uint8_t> bytes;
    for (const TwinaxFrame& f : msg) bytes.push_back(f.data);

    if (code == 0x00) {   // Activate Read
        if (!armed_read_) {
            fail(0x04);
            return false;
        }
        armed_read_ = false;
        busy_ = opt_.busy_polls;
        response.clear();
        for (size_t i = 0; i < read_data_.size(); ++i)
            response.push_back(TwinaxFrame{read_data_[i], uint8_t(i + 1 == read_data_.size() ? 7 : address_)});
        return !response.empty();
    }
    if (code == 0x01) {   // Activate Write
        if (!armed_write_) {
            fail(0x04);
            return false;
        }
        armed_write_ = false;
        busy_ = opt_.busy_polls;
        ++activate_writes_;
        if (!display()) {
            printed_.insert(printed_.end(), bytes.begin() + 1, bytes.end());
            return false;
        }
        for (size_t i = 1; i < bytes.size(); ++i) {
            if (ac_ >= ps_size()) {
                fail(0x0A);
                break;
            }
            ps_[ac_++] = bytes[i];
        }
        return false;
    }

    if (lpe_next_) {   // this load arrives with a parity error: it is lost
        lpe_next_ = false;
        lpe_ = true;
        armed_read_ = armed_write_ = false;
        exception_ = exception_ == 0x0E ? exception_ : 0;   // the 3477 clears its exception on an LPE
        return false;
    }
    run_load(bytes);
    return false;
}

void FakeStation::run_load(const std::vector<uint8_t>& b) {
    ++loads_;
    busy_ = opt_.busy_polls;
    if (b.size() > queue_size()) {
        fail(0x0A);
        return;
    }
    size_t i = 0;
    auto need = [&](size_t n) {
        if (i + n > b.size()) {
            fail(0x08);
            return false;
        }
        return true;
    };
    auto addr = [&]() { const unsigned v = unsigned(b[i]) << 8 | b[i + 1]; i += 2; return v; };
    while (i < b.size()) {
        const uint8_t c = b[i++];
        if (armed_read_ || armed_write_) {
            if ((c & 0x1F) != 0x02) fail(0x04);
            armed_read_ = armed_write_ = false;
            return;
        }
        if (exception_) {
            // Reset/EOQ always pass; power-on admits only Set Mode, any other
            // exception only WCD. Anything else ends the load silently.
            const bool pass = (c & 0x1F) == 0x02 || (exception_ == 0x0E && c == 0x13) ||
                              (exception_ != 0x0E && c == 0x05);
            if (!pass) return;
        }
        switch (c) {
        case 0x62: return;   // EOQ
        case 0x02: power_on(); return;
        case 0x13: {
            if (!need(1)) return;
            const uint8_t mode = b[i++];
            if ((mode & 3) == 1 || (mode & 3) == 2) { fail(0x08); return; }
            if (!set_mode_) cr_ = 0;
            set_mode_ = true;
            if (exception_ == 0x0E) exception_ = 0;
            break;
        }
        case 0x05: {
            if (!need(1)) return;
            const uint8_t b1 = b[i++];
            int b2 = -1, b3 = -1;
            if (display() && (b1 & 0x40)) {
                if (!need(1)) return;
                b2 = b[i++];
                if (b2 & 0x80) {
                    // A third byte, only after Read Info C0; before it the
                    // byte is taken as a command.
                    if (!native_info_) { fail(0x08); return; }
                    if (!need(1)) return;
                    b3 = b[i++];
                }
            }
            if (exception_ && !(b1 & 0x04)) break;   // ignored during an exception
            if (b1 & 0x04 && !bad_info_) exception_ = 0;
            wcd1_ = b1;
            if (b1 & 0x01) ++alarms_;
            if (b2 >= 0) {
                wcd2_ = uint8_t(b2);
                const bool wide = (b2 & 0x08) != 0;
                if (wide != wide_) {
                    wide_ = wide;
                    ps_.resize(ps_size(), 0);
                }
                queue64_ = (b2 & 0x02) != 0;
            }
            if (b3 >= 0) pointer_reporting_ = opt_.pointer && (b3 & 0x10) != 0;
            break;
        }
        case 0x0C:
            ++id_reads_;
            arm_read({display() ? uint8_t(0xC2) : opt_.printer_id});
            return;
        case 0x12:
            if (display()) {
                if (rc_ < ac_ || rc_ >= ps_size()) { fail(0x0C); return; }
                for (unsigned a = ac_; a <= rc_; ++a) ps_[a] = 0;
            }
            break;
        case 0x1E:
            armed_write_ = true;
            return;
        default:
            if (!display()) { fail(0x08); return; }
            switch (c) {
            case 0x45: {
                if (!need(1)) return;
                indicators_3477_ = b[i++];
                if (indicators_3477_ & 0x01) { if (!need(1)) return; ++i; }
                break;
            }
            case 0x15: if (!need(2)) return; ac_ = addr(); break;
            case 0x07: if (!need(2)) return; rc_ = addr(); break;
            case 0x17: if (!need(2)) return; cr_ = addr(); break;
            case 0x11: {
                if (!need(1)) return;
                const uint8_t first = b[i++];
                std::vector<uint8_t> data;
                if (first >= 1 && first <= queue_size() - 2) {
                    if (!need(first)) return;
                    data.assign(b.begin() + long(i), b.begin() + long(i + first));
                    i += first;
                } else {
                    data.push_back(first);
                }
                for (uint8_t d : data) {
                    if (ac_ >= ps_size()) { fail(0x0A); return; }
                    ps_[ac_++] = d;
                }
                cr_ = ac_;
                break;
            }
            case 0x51: if (!need(1)) return; indicators_ = b[i++]; break;
            case 0x2C: arm_read({opt_.keyboard_id}); return;
            case 0x8C: arm_read({opt_.model_id}); return;
            case 0x4C: arm_read({0x80}); return;
            case 0x88: {
                std::vector<uint8_t> d(16, 0);
                d[0] = opt_.model_id;
                arm_read(d);
                return;
            }
            case 0x0B:
                if (!need(1)) return;
                if (b[i] == 0xC0 && opt_.is_348x) {
                    std::vector<uint8_t> v = vpd();
                    v[2] = 0xF8; v[4] = 0xC8; v[5] = 0xC1;   // "3487HA0"
                    native_info_ = true;
                    arm_read(v);
                    return;
                }
                // A 3477 asked for anything but 80: an exception that
                // Write Control Data does not reset [obs]; the controller
                // must not ask.
                if (b[i] != 0x80 && opt_.is_3477) bad_info_ = true;
                if (b[i] != 0x80 || !opt_.is_3477) { fail(0x08); return; }
                arm_read(vpd());
                return;
            case 0x09:
                if (!need(2) || !opt_.is_348x || b[i] != 0x09 || b[i + 1] < 1 || b[i + 1] > 5) { fail(0x08); return; }
                record_ = b[i + 1];
                i += 2;
                break;
            case 0xBA: {
                // The pointer record between the counters; needs device
                // addressing and record 3.
                if (!opt_.is_348x || record_ != 3 || !(wcd2_ & 0x40) || rc_ < ac_ || rc_ >= 40) { fail(0x0C); return; }
                std::vector<uint8_t> rec(40, 0);
                if (!pointer_events_.empty()) {
                    std::copy(pointer_events_.front().begin(), pointer_events_.front().end(), rec.begin());
                    pointer_events_.pop_front();
                    ++pointer_read_;
                }
                pointer_offers_ = 0;
                arm_read(std::vector<uint8_t>(rec.begin() + ac_, rec.begin() + rc_ + 1));
                return;
            }
            default:
                fail(0x08);
                return;
            }
        }
    }
}

} // namespace test
} // namespace em34xx
