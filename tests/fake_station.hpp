// SPDX-License-Identifier: GPL-3.0-or-later
// A fake 5250 station for unit tests of the virtual workstation controller.
// It is written from the device-protocol description of the IBM 3477 (the
// project's device spec, itself based on the 5250 Product Attachment
// Information, Oct 1989), not from any firmware code. It models one station
// address: power-on status, Set Mode, the poll response (one frame before
// Set Mode, two after), the 16/64-byte queue, the exception latch and its
// gating, preactivate + Activate Read/Write, the display buffer commands the
// controller uses, the indicators, a 7-key keyboard queue with response
// levels, line parity errors, and a printer personality. As a 3486/3487
// (`is_348x`) it also has what docs/twinax-bridge.md, "Pointing device",
// records as observed: Read Info C0, the third Write Control Data byte, the
// record selection and read, and button events offered through outstanding
// status for two polls.
#pragma once

#include "host/host_config.hpp"
#include "host/twinax_link.hpp"

#include <cstdint>
#include <deque>
#include <vector>

namespace em34xx {
namespace test {

struct FakeStationOptions {
    StationType type = StationType::Display;
    uint8_t model_id = 0x24;        // 24 mono, 50 colour
    uint8_t keyboard_id = 0x82;     // Enhanced, US
    bool is_3477 = true;            // answers Read Info with "3477" VPD
    uint8_t printer_id = 0x2D;      // 5219 (3812 SCS emulation)
    unsigned busy_polls = 1;        // polls answered busy after each queue load / activate
    bool is_348x = false;           // answers Read Info C0 with "3487" VPD
    bool pointer = false;           // a pointing device is attached (348x only)
};

class FakeStation {
public:
    FakeStation(uint8_t address, FakeStationOptions opt = FakeStationOptions());

    // Controller -> station. Returns true with `response` filled when the
    // station answers (polls and Activate Read).
    bool receive(const TwinaxMessage& msg, TwinaxMessage& response);

    uint8_t address() const { return address_; }

    // Test controls.
    void power_on();                       // as after power-up or a Reset
    void set_powered(bool on);             // off: silent; on: power-on status
    void drop_responses(unsigned n) { drop_ = n; }
    void inject_parity_error() { lpe_next_ = true; }   // the next queue load arrives corrupted
    void latch_exception(uint8_t code);    // e.g. 0x0A overrun
    void press(uint8_t scan);              // into the 7-key queue
    // A button event: an event code per button (1 pressed, 2 released, 3
    // double click) and the pointer's buffer address. Only queued while the
    // controller has reporting on.
    void pointer_event(uint8_t left, uint8_t right, uint8_t middle, unsigned position);

    // Observed state.
    bool set_mode_done() const { return set_mode_; }
    uint8_t exception() const { return exception_; }
    unsigned rows() const { return wide_ ? 27 : 24; }
    unsigned cols() const { return wide_ ? 132 : 80; }
    unsigned ps_size() const { return wide_ ? 3696 : 2000; }
    const std::vector<uint8_t>& ps() const { return ps_; }
    uint8_t cell(unsigned row, unsigned col) const { return ps_[row * cols() + col]; }
    unsigned cursor() const { return cr_; }
    uint8_t indicators() const { return indicators_; }
    uint8_t indicators_3477() const { return indicators_3477_; }
    uint8_t wcd_byte1() const { return wcd1_; }
    uint8_t wcd_byte2() const { return wcd2_; }
    unsigned alarms() const { return alarms_; }
    size_t queue_size() const { return queue64_ ? 64 : 16; }
    const std::vector<uint8_t>& printed() const { return printed_; }
    unsigned loads() const { return loads_; }
    unsigned activate_writes() const { return activate_writes_; }   // accepted ones
    unsigned device_id_reads() const { return id_reads_; }          // Read Device ID commands
    bool pointer_reporting() const { return pointer_reporting_; }
    unsigned pointer_events_dropped() const { return pointer_dropped_; }   // offered twice, not read
    unsigned pointer_events_read() const { return pointer_read_; }
    uint8_t printer_status = 0;    // poll byte 2 of a printer

private:
    void run_load(const std::vector<uint8_t>& b);
    void fail(uint8_t exc);
    void arm_read(std::vector<uint8_t> data);
    std::vector<uint8_t> vpd() const;
    bool display() const { return opt_.type == StationType::Display; }

    uint8_t address_;
    FakeStationOptions opt_;
    bool powered_ = true;
    unsigned drop_ = 0;
    bool lpe_next_ = false;
    bool lpe_ = false;

    bool set_mode_ = false;
    uint8_t exception_ = 0x0E;
    unsigned busy_ = 0;
    uint8_t level_ = 0;
    std::deque<uint8_t> keys_;
    uint8_t key_reg_ = 0;
    bool key_shown_ = false;

    bool armed_read_ = false;
    bool armed_write_ = false;
    std::vector<uint8_t> read_data_;

    std::vector<uint8_t> ps_;
    unsigned ac_ = 0, rc_ = 0, cr_ = 0x4F;
    bool wide_ = false;
    bool queue64_ = false;
    uint8_t wcd1_ = 0, wcd2_ = 0;
    uint8_t indicators_ = 0, indicators_3477_ = 0;
    unsigned alarms_ = 0;
    unsigned loads_ = 0;
    unsigned activate_writes_ = 0;
    unsigned id_reads_ = 0;
    std::vector<uint8_t> printed_;

    bool native_info_ = false;       // Read Info C0 was processed
    bool bad_info_ = false;          // a 3477 was sent an unknown Read Info: stuck
    bool pointer_reporting_ = false;
    uint8_t record_ = 1;             // selected by 09 09 n
    std::deque<std::vector<uint8_t>> pointer_events_;
    unsigned pointer_offers_ = 0;    // polls that showed the front event
    unsigned pointer_dropped_ = 0;
    unsigned pointer_read_ = 0;
};

} // namespace test
} // namespace em34xx
