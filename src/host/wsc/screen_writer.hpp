// SPDX-License-Identifier: GPL-3.0-or-later
// The device mirror (what the controller believes the display holds) and
// the flush planner that turns (shadow screen - mirror) into device
// command steps.
#pragma once

#include "host/wsc/device_commands.hpp"
#include "host/wsc/session_port.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace em34xx {
namespace wsc {

struct DeviceMirror {
    static constexpr int16_t kUnknown = -1;

    unsigned rows = 0;             // 0 = format unknown
    unsigned cols = 0;
    std::vector<int16_t> cells;    // byte value, or kUnknown
    int cursor = -1;               // -1 = unknown
    int indicators = -1;           // WDLC-to-indicators byte
    int indicators_3477 = -1;      // WCD-to-indicators byte
    int wcd_byte1 = -1;            // persistent byte 1 bits (blink, clicker)
    int wcd_byte2 = -1;
    int wcd_byte3 = -1;            // 3486/3487 third byte, -1 if not sent

    size_t size() const { return size_t(rows) * cols; }
    void set_format(unsigned r, unsigned c);   // keeps nothing: all unknown
    void invalidate();                         // everything unknown, format kept
    bool matches(const ScreenState& s) const;  // cells, cursor and indicators
};

// One device operation: a queue load, optionally followed (after a clean
// poll) by an Activate Write with `payload`. `apply` updates the mirror
// once the station has executed the step cleanly.
struct Step {
    std::vector<uint8_t> load;
    std::vector<uint8_t> payload;
    std::function<void(DeviceMirror&)> apply;
    std::string what;
};

struct FlushOptions {
    size_t queue_bytes = kQueueBytes;   // per queue load
    unsigned merge_gap = 6;             // merge runs separated by fewer unchanged cells
    unsigned clear_percent = 60;        // clear first if this share of cells must become null
    uint8_t wcd_byte2_base = 0;         // 3477: wcd::B2Queue64
    bool has_3477_indicators = false;   // send WCD-to-indicators (45)
    int wcd_byte3 = -1;                 // 3486/3487: third WCD byte, -1 = two bytes
};

// Plans the steps that bring a device showing `mirror` to `shadow`.
// `alarm` adds a one-shot alarm to a Write Control Data step.
std::vector<Step> plan_flush(const ScreenState& shadow, const DeviceMirror& mirror, bool alarm,
                             const FlushOptions& opt);

// The WCD bytes that put a device into `shadow`'s format and control state.
uint8_t wcd_byte1_for(const ScreenState& shadow);
uint8_t wcd_byte2_for(const ScreenState& shadow, const FlushOptions& opt);

} // namespace wsc
} // namespace em34xx
