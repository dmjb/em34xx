// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/wsc/idle_host.hpp"

#include "util/log.hpp"
#include "util/strings.hpp"

namespace em34xx {
namespace wsc {

ScreenState LocalDisplayPort::blank_screen() {
    ScreenState s;
    s.rows = 24;
    s.cols = 80;
    s.cells.assign(size_t(s.rows) * s.cols, 0x00);
    s.cursor = 0;
    s.indicators = 0;   // input not inhibited
    return s;
}

void LocalDisplayPort::device_ready(const StationIdentity& id) {
    ready_ = true;
    ++generation_;   // the device shows a cleared screen: repaint
    LOG_INFO("wsc", "station %u on line (%s); local screen, no host session", unsigned(id.address),
             id.describe().c_str());
}

void LocalDisplayPort::move(int delta) {
    const int n = int(screen_.cells.size());
    screen_.cursor = unsigned(((int(screen_.cursor) + delta) % n + n) % n);
    ++generation_;
}

void LocalDisplayPort::put(uint8_t ebcdic) {
    screen_.cells[screen_.cursor] = ebcdic;
    move(1);
}

void LocalDisplayPort::operator_action(const OperatorAction& a) {
    switch (a.kind) {
    case OperatorAction::Kind::Character:
        put(a.ebcdic);
        break;
    case OperatorAction::Kind::Function:
        switch (a.function) {
        case KeyFunction::CursorLeft: move(-1); break;
        case KeyFunction::CursorRight: move(1); break;
        case KeyFunction::CursorUp: move(-int(screen_.cols)); break;
        case KeyFunction::CursorDown: move(int(screen_.cols)); break;
        case KeyFunction::Backspace: move(-1); screen_.cells[screen_.cursor] = 0x00; break;
        case KeyFunction::Home: screen_.cursor = 0; ++generation_; break;
        case KeyFunction::NewLine:
        case KeyFunction::FieldExit:
            move(int(screen_.cols - screen_.cursor % screen_.cols));
            break;
        default: break;
        }
        break;
    case OperatorAction::Kind::Aid:
        // Enter, F-keys: nothing to send them to; clear the screen.
        screen_ = blank_screen();
        ++generation_;
        break;
    default:
        break;
    }
}

// With no host there is no hot spot to act on: a left press only puts the
// cursor under the pointer, which shows that the events arrive.
void LocalDisplayPort::pointer_action(const PointerAction& a) {
    if (a.button != PointerAction::Button::Left || a.kind != PointerAction::Kind::Press) return;
    if (a.row >= screen_.rows || a.col >= screen_.cols) return;
    screen_.cursor = a.row * screen_.cols + a.col;
    ++generation_;
}

LocalController::LocalController(const std::vector<StationConfig>& stations, const WscTiming& timing)
    : LinkScheduler(stations, timing) {
    for (const auto& s : this->stations()) {
        if (s->configured_type() != StationType::Display) continue;
        ports_.push_back(std::make_unique<LocalDisplayPort>());
        s->attach_display(ports_.back().get());
    }
}

std::string LocalController::describe() const {
    std::string out = LinkScheduler::describe();
    out += "  display stations show a local screen (no TN5250 session)\n";
    return out;
}

std::unique_ptr<TwinaxHost> make_local_controller(const TwinaxConfig& twinax, std::string& error) {
    const std::vector<StationConfig> stations = twinax.effective_stations();
    if (stations.empty()) {
        error = "twinax host 'wsc' needs at least one station";
        return nullptr;
    }
    return std::make_unique<LocalController>(stations);
}

} // namespace wsc
} // namespace em34xx
