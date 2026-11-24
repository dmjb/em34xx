// SPDX-License-Identifier: GPL-3.0-or-later
// The virtual workstation controller on its own, without a TN5250 session:
// `--twinax-host wsc`. The controller discovers, identifies and polls every
// configured station, exactly as in the TN5250 bridge; display stations get
// a built-in local screen instead of a host session. It shows a blank screen
// with the keyboard unlocked and echoes typed characters, so the terminal
// can be brought on line and exercised without an IBM i system.
#pragma once

#include "host/host_config.hpp"
#include "host/twinax_link.hpp"
#include "host/wsc/link_scheduler.hpp"
#include "host/wsc/session_port.hpp"

#include <memory>
#include <string>
#include <vector>

namespace em34xx {
namespace wsc {

// A local display session: blank 24x80 screen, keyboard not inhibited,
// characters typed by the operator are shown at the cursor.
class LocalDisplayPort : public DisplaySessionPort {
public:
    void device_ready(const StationIdentity& id) override;
    void device_lost() override { ready_ = false; }
    uint64_t screen_generation() const override { return generation_; }
    void screen_snapshot(ScreenState& out) const override { out = screen_; }
    bool take_alarm() override { return false; }
    void operator_action(const OperatorAction& action) override;
    void pointer_action(const PointerAction& action) override;

    bool ready() const { return ready_; }
    const ScreenState& screen() const { return screen_; }

private:
    void put(uint8_t ebcdic);
    void move(int delta);

    ScreenState screen_ = blank_screen();
    uint64_t generation_ = 1;
    bool ready_ = false;

    static ScreenState blank_screen();
};

// LinkScheduler that owns a LocalDisplayPort for each display station.
class LocalController : public LinkScheduler {
public:
    LocalController(const std::vector<StationConfig>& stations, const WscTiming& timing = WscTiming());
    std::string describe() const override;

private:
    std::vector<std::unique_ptr<LocalDisplayPort>> ports_;
};

std::unique_ptr<TwinaxHost> make_local_controller(const TwinaxConfig& twinax, std::string& error);

} // namespace wsc
} // namespace em34xx
