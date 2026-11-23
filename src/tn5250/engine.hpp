// SPDX-License-Identifier: GPL-3.0-or-later
// 5250 display data stream engine: the shadow presentation space, format
// table, keyboard state and operator field editing of one display station.
//
// Written from the IBM 5494 Remote Control Unit Functions Reference
// (SC30-3533-04, "FR"), chapters 15 (data stream) and 16 (keyboard), and
// RFC 1205 section 5 (MC, TD, Read MDT Alternate, Read Modified Immediate
// Alternate, Query). Section numbers in comments refer to FR.
//
// The engine takes host records (opcode, flags, data) and produces inbound
// records through the DisplaySession record sink. It owns no I/O.
#pragma once

#include "tn5250/session_api.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace em34xx {
namespace tn5250 {

// Operator error codes the engine posts. FR SC30-3533-04 names only some of
// them (0005, 0012, 0019, 0034, 0037); the rest follow the IBM 5250
// operator error code numbering as we recall it from IBM operator
// documentation [hypothesis: verify against the 5494 User's Guide].
enum OperatorError : uint16_t {
    kErrProtected = 0x0005,         // cursor in protected area [doc FR 16.3.3.1]
    kErrAlphaOnly = 0x0008,         // alphabetic-only field [hyp]
    kErrNumericOnly = 0x0009,       // numeric-only field [hyp]
    kErrDigitsOnly = 0x0010,        // only 0-9 allowed [hyp]
    kErrSignPosition = 0x0011,      // key not valid in the sign position [hyp]
    kErrInsertFull = 0x0012,        // no room to insert [doc FR 15.6.12.2]
    kErrMandatoryFill = 0x0014,     // mandatory-fill field must be filled [hyp]
    kErrFieldMinus = 0x0016,        // Field- not allowed here [hyp]
    kErrExitKey = 0x0018,           // key used to exit field not valid [hyp]
    kErrDupNotAllowed = 0x0019,     // Dup / Field Mark not allowed [doc FR 16.3.3.2]
    kErrMandatoryEnter = 0x0021,    // mandatory-enter field must be entered [hyp]
};

// Negative-response sense codes (FR 13.4, chapter 15).
enum SenseCode : uint32_t {
    kSenseInvalidCommand = 0x10030101,
    kSenseInvalidClearAlt = 0x10030105,
    kSenseResequence = 0x10050103,
    kSenseSfLength = 0x10050110,
    kSenseSfClassType = 0x10050111,
    kSensePrematureEnd = 0x10050121,
    kSenseRowCol = 0x10050122,
    kSenseRaBackwards = 0x10050123,
    kSenseFieldLength = 0x10050125,
    kSenseFieldAddress = 0x10050126,
    kSenseRestoreData = 0x10050127,
    kSenseFieldPastEnd = 0x10050128,
    kSenseFormatOverflow = 0x10050129,
    kSenseWritePastEnd = 0x1005012A,
    kSenseSohLength = 0x1005012B,
    kSenseRollParam = 0x1005012C,
    kSenseExtAttrType = 0x1005012D,
    kSenseFieldAttr = 0x10050130,
    kSenseNoEscape = 0x10050131,
    kSenseWecwRowCol = 0x10050132,
};

struct EngineOptions {
    bool wide = true;                 // 27x132 supported (every 3477)
    bool color = true;                // IBM-3477-FC; false for -FG
    std::string machine_type = "3477";
    std::string model = "0FC";        // 3 characters
    uint8_t keyboard_id = 0x02;       // FR Table 89 "Keyboard ID"
    // Derive the above from an RFC 1091 terminal type such as IBM-3477-FG.
    static EngineOptions for_terminal_type(const std::string& ttype);
};

// FCWs accepted per input field [design]: FR 15.6.12.2 defines about a dozen
// kinds, and the save image stores the count in one byte. More give sense
// 10050129.
constexpr size_t kMaxFcws = 32;

struct Field {
    int start = 0;                    // address of the first data position
    int length = 0;
    uint16_t ffw = 0;
    std::vector<uint16_t> fcws;
    uint8_t attr = 0x20;

    bool bypass() const { return (ffw & 0x2000) != 0; }
    bool dup_enabled() const { return (ffw & 0x1000) != 0; }
    bool mdt() const { return (ffw & 0x0800) != 0; }
    unsigned shift() const { return (ffw >> 8) & 0x07; }
    bool auto_enter() const { return (ffw & 0x0080) != 0; }
    bool fer() const { return (ffw & 0x0040) != 0; }
    bool monocase() const { return (ffw & 0x0020) != 0; }
    bool mandatory_enter() const { return (ffw & 0x0008) != 0; }
    unsigned adjust() const { return ffw & 0x0007; }
    bool signed_numeric() const { return shift() == 7; }
    bool transparent() const;
    int fcw(uint8_t type) const;      // low byte of the first FCW of a type, or -1
    int end() const { return start + length; }               // one past the last position
    int last_enterable() const { return start + length - (signed_numeric() && length > 1 ? 2 : 1); }
};

class Engine : public DisplaySession {
public:
    explicit Engine(EngineOptions options = {});

    // Host side.
    void receive(const Record& rec);
    void set_system_available(bool on);

    // DisplaySession.
    void pump() override {}
    int rows() const override { return rows_; }
    int cols() const override { return cols_; }
    const Cell& cell(int row, int col) const override { return cells_[size_t(row * cols_ + col)]; }
    int cursor_row() const override { return cursor_ / cols_; }
    int cursor_col() const override { return cursor_ % cols_; }
    Indicators indicators() const override;
    uint64_t generation() const override { return generation_; }
    Rect take_dirty() override;
    unsigned take_alarms() override;
    void snapshot(ScreenSnapshot& out) const override;
    KeyResult key_data(uint8_t ebcdic) override;
    KeyResult key_function(Key key) override;
    KeyResult key_aid(Aid aid) override;
    KeyResult pointer_event(const PointerEvent& ev) override;
    void set_record_sink(RecordSink sink) override { sink_ = std::move(sink); }
    void set_device_ready(bool ready) override;
    bool device_ready() const { return device_ready_; }

    // Inspection, for tests and the probe.
    const std::vector<Field>& fields() const { return fields_; }
    int error_row() const;
    bool read_pending() const { return read_cmd_ != 0; }
    uint8_t pending_aid() const { return pending_aid_; }
    const EngineOptions& options() const { return opt_; }
    // Builds the 5250 Query Reply (FR 15.27.2 Table 88/89) with its
    // leading cursor address and AID.
    std::vector<uint8_t> query_reply() const;
    // Opaque Save Screen image (without the leading 04 12).
    std::vector<uint8_t> save_image() const;
    bool restore_image(const uint8_t* p, size_t n, size_t& used);

private:
    struct Error { uint32_t sense; };

    // Data stream commands.
    void process_stream(const std::vector<uint8_t>& d, uint8_t opcode);
    void cmd_clear_unit(int rows, int cols);
    void cmd_clear_format_table();
    void cmd_write_to_display(const std::vector<uint8_t>& d, size_t& i);
    void cmd_write_error_code(const std::vector<uint8_t>& d, size_t& i, bool window);
    void cmd_read(uint8_t cmd, uint8_t cc0, uint8_t cc1);
    void cmd_roll(uint8_t b1, uint8_t top, uint8_t bottom);
    void cmd_write_structured_field(const std::vector<uint8_t>& d, size_t& i);
    void apply_cc0(uint8_t cc0);
    void apply_cc1(uint8_t cc1, int cursor_order);
    void send_negative(uint32_t sense);
    void send(uint8_t opcode, uint8_t flags, std::vector<uint8_t> data);
    // Per-record limits; over the limit the record ends in a negative response.
    void count_reply();
    void count_cells(size_t n);

    // Screen primitives.
    void set_ch(int addr, uint8_t ch);
    void set_ext(int addr, uint8_t primary, uint8_t color);
    void set_cursor(int addr);
    void refresh_attrs();
    void changed() { ++generation_; }
    int size() const { return rows_ * cols_; }

    // Format table / keyboard helpers.
    int field_index_at(int addr) const;
    int home_address() const;
    int next_input_field(int from_addr, bool forward) const;
    void lock_keyboard();
    KeyResult post_error(uint16_t code);
    void restore_error_line();
    KeyResult field_exit(Key how);
    void advance_after_field(int field_idx);
    bool right_adjust(Field& f, uint8_t fill);
    KeyResult send_aid(Aid aid);
    std::vector<uint8_t> build_read_reply(uint8_t cmd, uint8_t aid, bool immediate) const;
    std::vector<uint8_t> field_data(const Field& f, bool keep_nulls, bool pad) const;
    std::vector<int> field_order() const;
    bool aid_includes_data(uint8_t aid) const;
    void set_mdt(Field& f) { f.ffw = uint16_t(f.ffw | 0x0800); }

    // Pointing device (pointer.cpp).
    std::string pointer_row_text(int row) const;
    KeyResult pointer_left_press(int addr);
    KeyResult pointer_hot_spot(int addr, bool right);
    KeyResult pointer_menu_choice(const std::string& number);

    EngineOptions opt_;
    RecordSink sink_;
    int rows_ = 24, cols_ = 80;
    std::vector<Cell> cells_;
    int cursor_ = 0;
    int ic_ = -1;                     // last IC order address, -1 if none
    std::vector<Field> fields_;
    // Start of Header (FR 15.6.9).
    uint8_t soh_flags_ = 0;
    uint8_t resequence_ = 0;
    int error_row_header_ = 0;        // 1-based, 0 = default
    uint32_t pf_no_data_ = 0;         // bit n-1 set: PF n returns no data

    // Keyboard state (FR 16.1).
    bool system_available_ = false;
    bool device_ready_ = true;
    bool locked_ = true;
    bool insert_ = false;
    bool blink_ = false;
    bool message_waiting_ = false;
    ErrorState error_ = ErrorState::None;
    uint16_t error_code_ = 0;
    bool locked_before_error_ = true;
    bool unlock_after_reset_ = false;
    std::vector<Cell> saved_error_line_;
    bool fer_wait_ = false;           // Field Exit required at the last position

    // Address where the last left-button press positioned the text cursor
    // (FR 17, left button pressed, step 9), -1 if it did something else: a
    // double click there is Enter.
    int pointer_positioned_ = -1;

    uint8_t read_cmd_ = 0;            // pending 42 / 52 / 82, or 0
    uint8_t pending_aid_ = 0;         // AID held until the next read
    int pending_aid_cursor_ = 0;

    unsigned record_replies_ = 0;     // replies sent for the current host record
    size_t record_cells_ = 0;         // cells touched by repeat-type operations

    uint64_t generation_ = 1;
    Rect dirty_;
    unsigned alarms_ = 0;
};

} // namespace tn5250
} // namespace em34xx
