// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/wsc/screen_writer.hpp"

#include "util/strings.hpp"

namespace em34xx {
namespace wsc {

void DeviceMirror::set_format(unsigned r, unsigned c) {
    rows = r;
    cols = c;
    cells.assign(size(), kUnknown);
    cursor = -1;
}

void DeviceMirror::invalidate() {
    std::fill(cells.begin(), cells.end(), kUnknown);
    cursor = -1;
    indicators = -1;
    indicators_3477 = -1;
}

bool DeviceMirror::matches(const ScreenState& s) const {
    if (rows != s.rows || cols != s.cols || cells.size() != s.cells.size()) return false;
    for (size_t i = 0; i < cells.size(); ++i)
        if (cells[i] != int16_t(s.cells[i])) return false;
    return cursor == int(s.cursor) && indicators == int(s.indicators);
}

uint8_t wcd_byte1_for(const ScreenState& shadow) {
    return uint8_t((shadow.cursor_blink ? wcd::BlinkCursor : 0) | (shadow.clicker ? 0 : wcd::ClickerOff));
}

uint8_t wcd_byte2_for(const ScreenState& shadow, const FlushOptions& opt) {
    return uint8_t(opt.wcd_byte2_base | (shadow.cols == 132 ? wcd::B2Wide : 0));
}

std::vector<Step> plan_flush(const ScreenState& shadow, const DeviceMirror& mirror, bool alarm,
                             const FlushOptions& opt) {
    std::vector<Step> steps;
    DeviceMirror m = mirror;   // projected state after the steps planned so far
    const size_t n = size_t(shadow.rows) * shadow.cols;
    if (shadow.cells.size() != n || n == 0) return steps;

    // 1. Format and control: Write Control Data.
    const uint8_t b1 = wcd_byte1_for(shadow);
    const uint8_t b2 = wcd_byte2_for(shadow, opt);
    const int b3 = opt.wcd_byte3;
    const bool format_changed = m.rows != shadow.rows || m.cols != shadow.cols;
    if (format_changed || alarm || m.wcd_byte1 != b1 || m.wcd_byte2 != b2 || m.wcd_byte3 != b3) {
        Step s;
        QueueLoad q;
        q.write_control(uint8_t(b1 | (alarm ? wcd::Alarm : 0)), b2, opt.wcd_byte3).end_of_queue();
        s.load = q.bytes();
        const unsigned r = shadow.rows, c = shadow.cols;
        s.apply = [=](DeviceMirror& d) {
            if (d.rows != r || d.cols != c) d.set_format(r, c);   // no clear on the device
            d.wcd_byte1 = b1;
            d.wcd_byte2 = b2;
            d.wcd_byte3 = b3;
        };
        s.what = format("wcd %02x %02x%s", b1, b2, alarm ? " alarm" : "");
        s.apply(m);
        steps.push_back(std::move(s));
    }

    // 2. Clear first when most of the screen must become null, or nothing is known.
    size_t to_null = 0, unknown = 0;
    for (size_t i = 0; i < n; ++i) {
        if (m.cells[i] == DeviceMirror::kUnknown) ++unknown;
        if (shadow.cells[i] == 0 && m.cells[i] != 0) ++to_null;
    }
    if (unknown == n || to_null * 100 >= n * opt.clear_percent) {
        Step s;
        QueueLoad q;
        q.load_address(0).load_reference(unsigned(n - 1)).clear().end_of_queue();
        s.load = q.bytes();
        s.apply = [n](DeviceMirror& d) {
            for (size_t i = 0; i < n && i < d.cells.size(); ++i) d.cells[i] = 0;
        };
        s.what = "clear";
        s.apply(m);
        steps.push_back(std::move(s));
    }

    // 3. Runs of changed cells, merged across short gaps.
    struct Run { size_t start, end; };   // inclusive
    std::vector<Run> runs;
    for (size_t i = 0; i < n; ++i) {
        if (m.cells[i] == int16_t(shadow.cells[i])) continue;
        if (!runs.empty() && i - runs.back().end <= size_t(opt.merge_gap) + 1) runs.back().end = i;
        else runs.push_back({i, i});
    }

    // Short runs go as LAC + WDLC (several per load while they fit); long
    // runs as LAC + Write Data and an Activate Write.
    const size_t short_max = opt.queue_bytes - 6;   // LAC(3) + WDLC(2) + EOQ(1)
    QueueLoad pending;
    std::vector<Run> pending_runs;
    auto flush_pending = [&]() {
        if (pending.empty()) return;
        pending.end_of_queue();
        Step s;
        s.load = pending.bytes();
        std::vector<std::pair<size_t, std::vector<uint8_t>>> writes;
        for (const Run& r : pending_runs)
            writes.emplace_back(r.start, std::vector<uint8_t>(shadow.cells.begin() + long(r.start),
                                                              shadow.cells.begin() + long(r.end + 1)));
        const size_t last_end = pending_runs.back().end;
        s.apply = [writes, last_end](DeviceMirror& d) {
            for (const auto& w : writes)
                for (size_t k = 0; k < w.second.size(); ++k) d.cells[w.first + k] = w.second[k];
            d.cursor = int(last_end + 1);   // WDLC leaves the cursor after the data
        };
        s.what = format("wdlc x%zu", pending_runs.size());
        steps.push_back(std::move(s));
        pending = QueueLoad();
        pending_runs.clear();
    };
    for (const Run& r : runs) {
        const size_t len = r.end - r.start + 1;
        const uint8_t* data = shadow.cells.data() + r.start;
        if (len <= short_max) {
            if (pending.size() + 5 + len + 1 > opt.queue_bytes) flush_pending();
            pending.load_address(unsigned(r.start)).write_data_load_cursor(data, len);
            pending_runs.push_back(r);
            continue;
        }
        flush_pending();
        Step s;
        QueueLoad q;
        q.load_address(unsigned(r.start)).write_data();
        s.load = q.bytes();
        s.payload.assign(data, data + len);
        const size_t start = r.start;
        std::vector<uint8_t> copy(s.payload);
        s.apply = [start, copy](DeviceMirror& d) {
            for (size_t k = 0; k < copy.size(); ++k) d.cells[start + k] = copy[k];
        };
        s.what = format("write %zu@%zu", len, r.start);
        steps.push_back(std::move(s));
    }
    flush_pending();
    for (Step& s : steps) {
        // Project the WDLC cursor moves so step 4 sees them.
        if (s.what.compare(0, 4, "wdlc") == 0) s.apply(m);
    }

    // 4. Cursor and indicators in one load.
    QueueLoad q;
    const bool cursor = m.cursor != int(shadow.cursor);
    const bool ind = m.indicators != int(shadow.indicators);
    const bool ind3477 = opt.has_3477_indicators && m.indicators_3477 != int(shadow.indicators_3477);
    if (cursor) q.load_cursor(shadow.cursor);
    if (ind) q.write_indicators(shadow.indicators);
    if (ind3477) q.write_control_indicators(shadow.indicators_3477);
    if (!q.empty()) {
        q.end_of_queue();
        Step s;
        s.load = q.bytes();
        const unsigned cur = shadow.cursor;
        const uint8_t ib = shadow.indicators, ib3 = shadow.indicators_3477;
        s.apply = [=](DeviceMirror& d) {
            if (cursor) d.cursor = int(cur);
            if (ind) d.indicators = ib;
            if (ind3477) d.indicators_3477 = ib3;
        };
        s.what = "cursor/indicators";
        steps.push_back(std::move(s));
    }
    return steps;
}

} // namespace wsc
} // namespace em34xx
