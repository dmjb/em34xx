// SPDX-License-Identifier: GPL-3.0-or-later
// Interface between the TN5250 side of the bridge and its consumer.
//
// The consumer is the virtual workstation controller (src/host/wsc/), which
// paints the emulated terminal from the shadow screen and feeds operator
// keystrokes back in. Nothing here knows about Twinax, device commands or
// scan codes: the shadow is the 5250 presentation space as the IBM 5494
// Functions Reference (SC30-3533-04, ch. 15-16) describes it, and the input
// entry points are the 5250 operator functions, not keys.
//
// Threading: a DisplaySession is single-threaded. The networked
// implementation (tn5250/client.hpp) receives records on its own I/O thread
// and only applies them inside pump(), which the owner calls from the thread
// that also reads the shadow and delivers keys (the emulation thread).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace em34xx {
namespace tn5250 {

// ---------------------------------------------------------------------------
// Shadow screen

// One presentation-space position.
//
// `ch` is the byte held in the regeneration buffer: 0x00 null, 0x20-0x3F a
// field attribute (it occupies the position and shows as a blank), anything
// else an EBCDIC character (0x1C Dup and 0x1E Field Mark show as overstruck
// '*' and ';' on a real station). `attr` is the field attribute in effect at
// this position, i.e. the nearest attribute byte at or before it in reading
// order (0x20 "normal" when there is none). `ext_primary` and `ext_color`
// are the Write Extended Attribute values (FR 15.6.11.1/2; 0 = none).
//
// Extended attributes and the 3477. The 3477 has three extended character
// buffers (spec-3477-device-protocol.md section 2: device address 60h ECB,
// E0h Both; in extended mode each position is PS char + primary, text and
// colour ECB), and the cell keeps one plane for each:
//   ext_primary  primary ECB: highlighting, alternate character sets and
//                graphics; from WEA type 01 (FR 15.6.11.1)
//   ext_text     text ECB: word processing / text assist (the 3477 has a
//                text-assist WCD bit); fed by the WP-mode data stream (WEA
//                type 02, text-support structured fields FR 15.27.4), none
//                of which the engine implements yet, so it stays 0
//   ext_color    colour ECB: per-character colour; from WEA type 03
//                (FR 15.6.11.2)
// Decision: the Query Reply does NOT advertise extended attribute or text
// support (capability bytes 2 and 4 stay 0) until the controller actually
// drives the ECB; it paints in basic mode today. A conforming host then
// sends no WEA and the planes stay 0. When the controller gains ECB
// painting, set the matching bits in Engine::query_reply() in the same
// change.
struct Cell {
    uint8_t ch = 0x00;
    uint8_t attr = 0x20;
    uint8_t ext_primary = 0;
    uint8_t ext_color = 0;
    uint8_t ext_text = 0;            // text ECB; always 0 today (see above)

    bool is_attribute() const { return ch >= 0x20 && ch <= 0x3F; }
    bool operator==(const Cell& o) const {
        return ch == o.ch && attr == o.attr && ext_primary == o.ext_primary && ext_color == o.ext_color &&
               ext_text == o.ext_text;
    }
    bool operator!=(const Cell& o) const { return !(*this == o); }
};

// Field-attribute helpers (FR Table 51; bits counted with 0x80 = bit 0).
inline bool attr_column_separator(uint8_t a) { return (a & 0x10) != 0; }
inline bool attr_blink(uint8_t a) { return (a & 0x08) != 0 && (a & 0x07) != 0x07; }
inline bool attr_underscore(uint8_t a) { return (a & 0x04) != 0 && (a & 0x07) != 0x07; }
inline bool attr_high_intensity(uint8_t a) { return (a & 0x02) != 0 && (a & 0x07) != 0x07; }
inline bool attr_reverse(uint8_t a) { return (a & 0x01) != 0 && (a & 0x07) != 0x07; }
inline bool attr_nondisplay(uint8_t a) { return (a & 0x07) == 0x07; }

// Rows/columns are 0-based throughout this interface; the data stream's own
// 1-based addresses never leak out of the engine.
struct Rect {
    int top = 0, left = 0, bottom = -1, right = -1;  // inclusive; empty if bottom < top
    bool empty() const { return bottom < top || right < left; }
    void add(int row, int col) {
        if (empty()) { top = bottom = row; left = right = col; return; }
        if (row < top) top = row;
        if (row > bottom) bottom = row;
        if (col < left) left = col;
        if (col > right) right = col;
    }
};

// Operator error states (FR 16.1). PreHelp is entered by a keying error or
// a host Write Error Code; Help moves it to PostHelp; Reset leaves both.
enum class ErrorState : uint8_t { None, PreHelp, PostHelp };

struct Indicators {
    bool system_available = false;   // session up (5250 "System Available")
    bool message_waiting = false;
    bool input_inhibited = true;     // keyboard locked or in error state
    bool keyboard_locked = true;     // "normal locked" (waiting for the host)
    bool insert_mode = false;
    bool cursor_blink = false;       // WTD CC1 / error state cursor blink
    ErrorState error = ErrorState::None;
    uint16_t error_code = 0;         // operator error code (BCD-style, e.g. 0x0005)
    bool system_request = false;     // Sys Req line active (not used yet: Sys Req
                                     // is sent to the host at once, RFC 1205 4.3)
    // Keyboard clicker (FR 16.1): on in normal unlocked state, off when
    // locked or in an error state.
    bool clicker = false;
    // Type-ahead (FR 16.3.1) is not implemented; keys typed while locked
    // are ignored, so this stays false. TODO(type-ahead): buffer keys while
    // locked and drive the 3477 Type-Ahead indicator from it.
    bool type_ahead = false;
    // Caps Lock is not session state: the controller owns shift/caps
    // (it maps scan codes to EBCDIC) and drives the 3477 Caps Lock
    // indicator itself.
};

struct ScreenSnapshot {
    int rows = 24, cols = 80;
    std::vector<Cell> cells;         // rows*cols, row-major
    int cursor_row = 0, cursor_col = 0;
    Indicators indicators;
    uint64_t generation = 0;
    const Cell& at(int r, int c) const { return cells[size_t(r) * size_t(cols) + size_t(c)]; }
};

// ---------------------------------------------------------------------------
// Operator input

// Attention identifier codes (FR Table 109).
enum class Aid : uint8_t {
    None = 0x00,
    Clear = 0xBD,
    Enter = 0xF1,
    Help = 0xF3,
    RollDown = 0xF4,          // Page Up
    RollUp = 0xF5,            // Page Down
    RollLeft = 0xD9,
    RollRight = 0xDA,
    Print = 0xF6,
    RecordBackspace = 0xF8,
    AutoEnterSlp = 0x3F,
    ForwardEdgeTrigger = 0x50,
    PA1 = 0x6C,
    PA2 = 0x6E,
    PA3 = 0x6B,
    F1 = 0x31, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    F13 = 0xB1, F14, F15, F16, F17, F18, F19, F20, F21, F22, F23, F24,
    QueryReply = 0x88,        // inbound only
};
// F1..F24 as an Aid; n outside 1..24 gives Aid::None.
inline Aid function_key_aid(int n) {
    if (n >= 1 && n <= 12) return Aid(0x30 + n);
    if (n >= 13 && n <= 24) return Aid(0xB0 + (n - 12));
    return Aid::None;
}

// Non-AID operator functions (FR 16.2.2-16.3).
enum class Key : uint8_t {
    FieldExit,
    FieldPlus,
    FieldMinus,
    FieldAdvance,      // Tab
    FieldBackspace,    // Backtab
    CursorUp,
    CursorDown,
    CursorLeft,
    CursorRight,
    Backspace,         // destructive-free cursor left within input
    Home,
    NewLine,
    Insert,            // toggles insert mode
    Delete,
    EraseEof,
    EraseInput,
    Dup,
    FieldMark,
    Reset,             // Error Reset
    Help,              // Help: AID F3 normally, error help in error state
    Attention,
    SysReq,
};

inline const char* key_name(Key k) {
    switch (k) {
    case Key::FieldExit: return "FieldExit";
    case Key::FieldPlus: return "Field+";
    case Key::FieldMinus: return "Field-";
    case Key::FieldAdvance: return "FieldAdvance";
    case Key::FieldBackspace: return "FieldBackspace";
    case Key::CursorUp: return "Up";
    case Key::CursorDown: return "Down";
    case Key::CursorLeft: return "Left";
    case Key::CursorRight: return "Right";
    case Key::Backspace: return "Backspace";
    case Key::Home: return "Home";
    case Key::NewLine: return "NewLine";
    case Key::Insert: return "Insert";
    case Key::Delete: return "Delete";
    case Key::EraseEof: return "EraseEOF";
    case Key::EraseInput: return "EraseInput";
    case Key::Dup: return "Dup";
    case Key::FieldMark: return "FieldMark";
    case Key::Reset: return "Reset";
    case Key::Help: return "Help";
    case Key::Attention: return "Attention";
    case Key::SysReq: return "SysReq";
    }
    return "?";
}

// What the engine did with a key, so the controller can sound the alarm or
// repaint selectively.
enum class KeyResult : uint8_t {
    Accepted,          // state changed or an AID/signal was sent
    Ignored,           // e.g. data key while the keyboard is locked
    Error,             // operator error posted (see Indicators::error_code);
                       // an alarm has been queued for take_alarms()
};

// Pointing-device events as the display reports them to its controller (FR
// chapter 17): a button pressed, released or double-clicked, with the
// keyboard's Shift state and the character position under the pointer.
enum class PointerButton : uint8_t { Left, Right, Middle };
enum class PointerAction : uint8_t { Press, Release, DoubleClick };
struct PointerEvent {
    PointerButton button = PointerButton::Left;
    PointerAction action = PointerAction::Press;
    bool shift = false;
    int row = 0, col = 0;            // 0-based
};

// ---------------------------------------------------------------------------
// Records exchanged with the host (RFC 1205 section 3 header, minus framing)

enum RecordFlags : uint8_t {
    kFlagErr = 0x80,   // data stream output error / negative response
    kFlagAtn = 0x40,   // Attention key
    kFlagSrq = 0x04,   // System Request key
    kFlagTrq = 0x02,   // Test Request key
    kFlagHlp = 0x01,   // Help in error state
};

enum Opcode : uint8_t {
    kOpNoOp = 0x00,
    kOpInvite = 0x01,
    kOpOutputOnly = 0x02,
    kOpPutGet = 0x03,
    kOpSaveScreen = 0x04,
    kOpRestoreScreen = 0x05,
    kOpReadImmediate = 0x06,
    kOpReadScreen = 0x08,
    kOpCancelInvite = 0x0A,
    kOpMessageOn = 0x0B,
    kOpMessageOff = 0x0C,
};

struct Record {
    uint8_t opcode = kOpNoOp;
    uint8_t flags = 0;
    std::vector<uint8_t> data;       // 5250 data stream after the header
};

// ---------------------------------------------------------------------------
// The session as the controller sees it

class DisplaySession {
public:
    virtual ~DisplaySession() = default;

    // Apply anything that arrived from the host. Must be called regularly
    // from the consumer thread; cheap when there is nothing to do.
    virtual void pump() = 0;

    // --- shadow screen ---
    virtual int rows() const = 0;
    virtual int cols() const = 0;
    virtual const Cell& cell(int row, int col) const = 0;
    virtual int cursor_row() const = 0;
    virtual int cursor_col() const = 0;
    virtual Indicators indicators() const = 0;
    // Bumped on every change to cells, cursor, size or indicators.
    virtual uint64_t generation() const = 0;
    // Region changed since the last call (cells only; a size change marks
    // the whole screen). Clears the accumulated region.
    virtual Rect take_dirty() = 0;
    // Number of alarms requested since the last call. Two sources: WTD
    // CC1 "sound alarm" (FR Table 44), and every operator error the engine
    // posts (one alarm per KeyResult::Error), as a 5494 sounds the alarm on
    // entering pre-help error state. The controller turns each into a
    // device alarm.
    virtual unsigned take_alarms() = 0;
    virtual void snapshot(ScreenSnapshot& out) const = 0;

    // --- operator input ---
    virtual KeyResult key_data(uint8_t ebcdic) = 0;
    virtual KeyResult key_function(Key key) = 0;
    virtual KeyResult key_aid(Aid aid) = 0;
    // The controller's default processing of a mouse button event (FR
    // chapter 17).
    virtual KeyResult pointer_event(const PointerEvent& ev) = 0;

    // --- host replies ---
    // The engine produces inbound records (AID replies, query replies,
    // Attention / Sys Req signals, negative responses). A networked session
    // sends them itself; a bare engine hands them to this sink, which may be
    // replaced (for example by a test harness).
    using RecordSink = std::function<void(const Record&)>;
    virtual void set_record_sink(RecordSink sink) = 0;

    // --- device presence ---
    // The controller reports whether the display station answers on the
    // cable. TN5250 has no power-on/off notification for a display (SNA's
    // LUSTAT does not exist on telnet), so the host is not told; the
    // session stays up. On ready-after-lost the engine marks the whole
    // screen dirty and bumps the generation so the controller repaints.
    // TODO(device-lost policy): optionally end the telnet session after a
    // configurable time without the device, so the host frees the job.
    virtual void set_device_ready(bool ready) { (void)ready; }
};

// ---------------------------------------------------------------------------
// Printer stations (not implemented)
//
// TODO(printer): a TN5250 printer session (RFC 4777 sections 8-10:
// terminal type IBM-3812-1 etc., the startup response record, SCS data in
// Print records, and the client's acknowledgement / error records). The
// shape the controller would consume:
class PrinterSession {
public:
    virtual ~PrinterSession() = default;
    virtual void pump() = 0;
    // Next chunk of SCS data for the printer; false if none is queued.
    virtual bool take_scs(std::vector<uint8_t>& out) = 0;
    // The printer accepted everything handed out so far.
    virtual void acknowledge() = 0;
    // The printer reported a problem (intervention required, paper out...);
    // `code` is the 5250 printer status to forward to the host.
    virtual void report_status(uint16_t code) = 0;
    virtual void set_device_ready(bool ready) = 0;
};

} // namespace tn5250
} // namespace em34xx
