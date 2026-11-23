// SPDX-License-Identifier: GPL-3.0-or-later
// What the virtual workstation controller needs from the layer above it
// (the 5250 data-stream engine, src/tn5250/), per station address.
//
// This is a local seam. The engine's interface is tn5250::DisplaySession
// (src/tn5250/session_api.hpp); tn5250_adapter.hpp connects it to these
// ports. Keeping the seam lets either side evolve and lets tests use small
// doubles. The printer port has no engine counterpart yet.
#pragma once

#include "host/host_config.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace em34xx {
namespace wsc {

// What discovery learned about the device on one station address.
struct StationIdentity {
    uint8_t address = 0;
    StationType type = StationType::Display;   // what the device reported
    uint8_t device_id = 0;        // Read Device ID (base): C2 display, else a printer ID word
    // Display only:
    uint8_t model_id = 0;         // Read Device ID (model, 8C): 24 mono / 50 colour on a 3477
    bool has_keyboard_id = false;
    uint8_t keyboard_id = 0;      // 02 typewriter / 0A World Trade, |80 Enhanced keyboard
    bool has_extended_id = false;
    std::array<uint8_t, 16> extended_id{};  // Read Data to model (88): model, style, language, ...
    std::vector<uint8_t> vpd;     // Read Info (0B 80): 64 bytes, empty if refused

    bool is_3477() const;         // VPD starts with EBCDIC "3477"
    bool colour() const;          // model ID 50
    bool wide_capable() const;    // 27x132 presentation space
    bool enhanced_keyboard() const { return has_keyboard_id && (keyboard_id & 0x80); }
    std::string describe() const;
};

// A printer ID word: model (2 bits) | station type (2 bits, 10 = printer) |
// printer type (4 bits) [doc PAI 3.5.2.1 fig. 74].
bool device_id_is_printer(uint8_t id);
const char* printer_type_name(uint8_t id);   // "5256", "4214-2", "5219", or "?"

// The display's screen as the engine wants it shown. Cells are device codes:
// 00 null, 20-3F field attribute, 40-FE EBCDIC characters.
struct ScreenState {
    unsigned rows = 24;           // 24 (24x80) or 27 (27x132)
    unsigned cols = 80;
    std::vector<uint8_t> cells;   // rows * cols, row major
    unsigned cursor = 0;          // linear address
    uint8_t indicators = 0x02;    // WDLC-to-indicators bits: 80 message waiting,
                                  // 20 shift, 10 katakana, 08 insert, 04 diacritic,
                                  // 02 input inhibited
    uint8_t indicators_3477 = 0;  // WCD-to-indicators (45) bits: 80 caps lock,
                                  // 40 layer 100, 10 type-ahead (3477 only)
    bool cursor_blink = false;
    bool clicker = true;
};

// A keystroke after scan-code translation (keyboard_map.hpp).
enum class KeyFunction : uint8_t {
    None,
    FieldExit, FieldPlus, FieldMinus, NewLine,
    Tab, Backtab, Backspace,
    CursorUp, CursorDown, CursorLeft, CursorRight, Home,
    Insert, Delete, Dup, EraseEof, EraseInput, Reset,
    FieldMark, Hex, Record, Play, Test, SetCp,
};

const char* key_function_name(KeyFunction f);

struct OperatorAction {
    enum class Kind : uint8_t {
        Character,   // `ebcdic` is a data character
        Function,    // an editing or local function (`function`)
        Aid,         // an AID-generating key (`aid`: F1 Enter, 31-3C F1-F12, ...)
        Attn,        // signal keys
        SysReq,
        Overrun,     // the terminal's keyboard queue overflowed (scan FF)
        Unmapped,    // a scan code this keyboard map does not translate
    };
    Kind kind = Kind::Unmapped;
    uint8_t ebcdic = 0;
    KeyFunction function = KeyFunction::None;
    uint8_t aid = 0;
    uint8_t scan = 0;            // raw 5250 scan code (make)
    bool shift = false;
    bool alt = false;
};

const char* operator_action_name(OperatorAction::Kind k);

// A mouse button event from the display: which button, what happened, the
// keyboard's Shift state and the character position of the pointer.
struct PointerAction {
    enum class Button : uint8_t { Left, Right, Middle };
    enum class Kind : uint8_t { Press, Release, DoubleClick };
    Button button = Button::Left;
    Kind kind = Kind::Press;
    bool shift = false;
    unsigned row = 0, col = 0;   // 0-based
};

class DisplaySessionPort {
public:
    virtual ~DisplaySessionPort() = default;
    // Called as emulated time advances: apply host input (tn5250 pump()).
    virtual void pump() {}
    // Identification and initialisation are complete; the device shows a
    // cleared screen. The engine should expect a full repaint from its
    // shadow (no action needed: the controller diffs against its mirror).
    virtual void device_ready(const StationIdentity& id) = 0;
    // The device stopped answering, or reset itself.
    virtual void device_lost() = 0;
    // Bumped whenever anything in the screen state changes.
    virtual uint64_t screen_generation() const = 0;
    virtual void screen_snapshot(ScreenState& out) const = 0;
    // One-shot alarm request (WCD alarm bit); returns true once per request.
    virtual bool take_alarm() = 0;
    // A keystroke from the operator.
    virtual void operator_action(const OperatorAction& action) = 0;
    // A mouse button event; a session without mouse handling ignores it.
    virtual void pointer_action(const PointerAction& action) { (void)action; }
};

// Printer sessions. Discovery and identification are complete; the data
// path is an extension point: the engine hands over SCS byte strings and
// the controller transfers them with Write Data / Activate Write.
class PrinterSessionPort {
public:
    virtual ~PrinterSessionPort() = default;
    virtual void device_ready(const StationIdentity& id) = 0;
    virtual void device_lost() = 0;
    // SCS data to print; return false when there is none.
    virtual bool next_print_data(std::vector<uint8_t>& scs) = 0;
    // The printer's second poll-response byte, whenever it changes
    // [doc PAI 3.5.1.1.2: invalid SCS code/parameter, buffers full,
    // print complete, cancel, not ready, end of forms, graphic check].
    virtual void printer_status(uint8_t byte2) = 0;
};

} // namespace wsc
} // namespace em34xx
