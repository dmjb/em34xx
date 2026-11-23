// SPDX-License-Identifier: GPL-3.0-or-later
// Connects a tn5250::DisplaySession (the 5250 data-stream engine,
// src/tn5250/session_api.hpp) to a station's DisplaySessionPort.
#pragma once

#include "host/wsc/session_port.hpp"
#include "tn5250/session_api.hpp"

namespace em34xx {
namespace wsc {

class Tn5250DisplayAdapter : public DisplaySessionPort {
public:
    explicit Tn5250DisplayAdapter(tn5250::DisplaySession& session) : session_(session) {}

    void pump() override { session_.pump(); }
    void device_ready(const StationIdentity& id) override { identity_ = id; ready_ = true; }
    void device_lost() override { ready_ = false; }
    uint64_t screen_generation() const override { return session_.generation(); }
    void screen_snapshot(ScreenState& out) const override;
    bool take_alarm() override { return session_.take_alarms() > 0; }
    void operator_action(const OperatorAction& action) override;
    void pointer_action(const PointerAction& action) override;

    bool device_ready() const { return ready_; }
    const StationIdentity& identity() const { return identity_; }

private:
    tn5250::DisplaySession& session_;
    StationIdentity identity_;
    bool ready_ = false;
};

// The WDLC-to-indicators byte for the engine's indicator state.
uint8_t indicator_bits(const tn5250::Indicators& ind);

} // namespace wsc
} // namespace em34xx
