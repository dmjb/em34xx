// SPDX-License-Identifier: GPL-3.0-or-later
// The controller's default processing of mouse button events (FR chapter
// 17, "Display Pointer Device (Mouse) Support"). The display reports a
// button event and the pointer's position; everything the click then does
// is decided here, as a 5494 does it, and reaches the host as an ordinary
// AID or not at all. See docs/tn5250.md, "Pointing device".
//
// Implemented: the processing exceptions, operator-error reset, the mouse
// selection entry field (FCW 8Ann), the hot spots, text-cursor positioning
// and the double-click Enter. Not implemented: selection fields and scroll
// bars (no Write Data Structured Field yet), light-pen fields, programmable
// mouse buttons, copy and paste (the shifted events).
#include "tn5250/engine.hpp"

#include <cctype>
#include <string>

namespace em34xx {
namespace tn5250 {

namespace {

// The characters the hot spots are made of, as ASCII; everything else that
// shows is '?', and nulls, blanks, attributes and non-display data are ' '.
// All of them are in the EBCDIC invariant set, so the code page does not
// matter.
char hot_spot_char(uint8_t b) {
    if (b >= 0xF0 && b <= 0xF9) return char('0' + (b - 0xF0));
    if (b >= 0xC1 && b <= 0xC9) return char('A' + (b - 0xC1));
    if (b >= 0xD1 && b <= 0xD9) return char('J' + (b - 0xD1));
    if (b >= 0xE2 && b <= 0xE9) return char('S' + (b - 0xE2));
    if (b >= 0x81 && b <= 0x89) return char('a' + (b - 0x81));
    if (b >= 0x91 && b <= 0x99) return char('j' + (b - 0x91));
    if (b >= 0xA2 && b <= 0xA9) return char('s' + (b - 0xA2));
    switch (b) {
    case 0x00: case 0x40: return ' ';
    case 0x4B: return '.';
    case 0x4E: return '+';
    case 0x60: return '-';
    case 0x7E: return '=';
    case 0x61: return '/';
    case 0x7A: return ':';
    default: return '?';
    }
}

bool alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }
bool digit(char c) { return c >= '0' && c <= '9'; }

// Last position of a piece of text that starts at `from`: it runs to the
// position before two blanks in a row, before `stop`, or to the end.
int text_end(const std::string& s, int from, int stop) {
    int end = from;
    for (int k = from; k < stop; ++k) {
        if (s[size_t(k)] != ' ') end = k;
        else if (k + 1 >= stop || s[size_t(k + 1)] == ' ') break;
    }
    return end;
}

// "Fnn=" at position i: the key number, and j = the position of the '='.
int function_key_label(const std::string& s, int i, int& j) {
    const int n = int(s.size());
    if (s[size_t(i)] != 'F' || (i > 0 && alnum(s[size_t(i - 1)]))) return 0;
    int k = i + 1, value = 0;
    while (k < n && digit(s[size_t(k)]) && k - i <= 2) value = value * 10 + (s[size_t(k++)] - '0');
    if (k == i + 1 || k >= n || s[size_t(k)] != '=') return 0;
    j = k;
    return value;
}

// The command key whose description ("F12=Cancel") lies under column c,
// or 0.
int function_key_at(const std::string& s, int c) {
    const int n = int(s.size());
    int found = 0;
    for (int i = 0; i <= c; ++i) {
        int j = 0;
        const int key = function_key_label(s, i, j);
        if (!key) continue;
        int stop = n;   // the next key's description ends this one
        for (int k = j + 1; k < n; ++k) {
            int jj = 0;
            if (function_key_label(s, k, jj)) {
                stop = k;
                break;
            }
        }
        if (c <= text_end(s, i, stop)) found = key;
    }
    return found;
}

// The word (letters only) under column c, in lower case.
std::string word_at(const std::string& s, int c) {
    auto alpha = [&](int k) { return std::isalpha(static_cast<unsigned char>(s[size_t(k)])) != 0; };
    if (!alpha(c)) return {};
    int a = c, b = c;
    while (a > 0 && alpha(a - 1)) --a;
    while (b + 1 < int(s.size()) && alpha(b + 1)) ++b;
    std::string w;
    for (int k = a; k <= b; ++k) w += char(std::tolower(static_cast<unsigned char>(s[size_t(k)])));
    return w;
}

// A scrolling '+' or '-' under column c: it stands alone, or follows
// "More" as in "More-/+"; a sign or hyphen inside text does not count.
char scroll_sign_at(const std::string& s, int c) {
    const char x = s[size_t(c)];
    if (x != '+' && x != '-') return 0;
    if (c + 1 < int(s.size()) && alnum(s[size_t(c + 1)])) return 0;
    if (c > 0 && alnum(s[size_t(c - 1)]) && word_at(s, c - 1) != "more") return 0;
    return x;
}

// The number of the menu choice ("2. Office Tasks") under column c, or
// empty: 1 to 5 digits and a period, after a blank or at the start of the
// row.
std::string menu_choice_at(const std::string& s, int c) {
    const int n = int(s.size());
    std::string found;
    for (int i = 0; i <= c; ++i) {
        if (!digit(s[size_t(i)]) || (i > 0 && s[size_t(i - 1)] != ' ')) continue;
        int j = i;
        while (j < n && digit(s[size_t(j)])) ++j;
        if (j - i > 5 || j >= n || s[size_t(j)] != '.') continue;
        if (j + 1 < n && s[size_t(j + 1)] != ' ') continue;
        // The choice's text follows the period after one or two blanks.
        int text = j + 1;
        while (text < n && text <= j + 2 && s[size_t(text)] == ' ') ++text;
        const int end = text < n && s[size_t(text)] != ' ' ? text_end(s, text, n) : j;
        if (c <= end) found = s.substr(size_t(i), size_t(j - i));
    }
    return found;
}

} // namespace

std::string Engine::pointer_row_text(int row) const {
    std::string s(size_t(cols_), ' ');
    for (int c = 0; c < cols_; ++c) {
        const Cell& x = cells_[size_t(row * cols_ + c)];
        if (!x.is_attribute() && !attr_nondisplay(x.attr)) s[size_t(c)] = hot_spot_char(x.ch);
    }
    return s;
}

KeyResult Engine::pointer_event(const PointerEvent& ev) {
    if (ev.row < 0 || ev.row >= rows_ || ev.col < 0 || ev.col >= cols_) return KeyResult::Ignored;
    const int addr = ev.row * cols_ + ev.col;
    const bool left_press = ev.button == PointerButton::Left && ev.action == PointerAction::Press;
    const int positioned = pointer_positioned_;
    if (left_press || ev.action != PointerAction::Release) pointer_positioned_ = -1;

    // Operator error state: only a left press on the error line acts, and
    // it is Reset (FR 17, left button pressed, step 1; shifted too).
    if (error_ != ErrorState::None) {
        if (left_press && ev.row == error_row()) return key_function(Key::Reset);
        return KeyResult::Ignored;
    }
    // FR 17, "Mouse Button Event Processing Exceptions": keyboard locked,
    // or no read pending. (System request state, type-ahead and WP mode do
    // not exist in this engine.)
    if (locked_ || !read_cmd_) return KeyResult::Ignored;
    // The shifted events are copy and paste, which is not implemented.
    if (ev.shift) return KeyResult::Ignored;

    if (left_press) return pointer_left_press(addr);
    if (ev.button == PointerButton::Left && ev.action == PointerAction::DoubleClick) {
        // Enter, if the press before it positioned the text cursor (step 2).
        if (positioned >= 0 && positioned == cursor_) return key_aid(Aid::Enter);
        return KeyResult::Ignored;
    }
    if (ev.button == PointerButton::Right && ev.action == PointerAction::Press)
        return pointer_hot_spot(addr, true);
    return KeyResult::Ignored;
}

// FR 17, "Unshifted Left Button Pressed Event Processing". Steps 2, 3, 5,
// 6 and 8 need functions this engine does not have.
KeyResult Engine::pointer_left_press(int addr) {
    const int fi = field_index_at(addr);
    if (fi >= 0 && !fields_[size_t(fi)].bypass()) {
        // Step 4, mouse selection entry field (FR 15.6.12.2, FCW 8Ann): the
        // text cursor moves to the pointer and AID nn is returned.
        const int aid = fields_[size_t(fi)].fcw(0x8A);
        if (aid >= 0) {
            set_cursor(addr);
            return key_aid(Aid(aid));
        }
    } else {
        // Step 7: hot spots, only outside an entry field.
        const KeyResult r = pointer_hot_spot(addr, false);
        if (r != KeyResult::Ignored) return r;
    }
    // Step 9: the text cursor goes to the pointer, wherever that is.
    set_cursor(addr);
    pointer_positioned_ = addr;
    return KeyResult::Accepted;
}

// The hot spots (FR 17, left button pressed, step 7, and right button
// pressed, step 2). What exactly counts as command key text, a scrolling
// sign or a menu choice is not specified; the patterns here are a [design]
// reading of the examples.
KeyResult Engine::pointer_hot_spot(int addr, bool right) {
    const std::string s = pointer_row_text(addr / cols_);
    const int c = addr % cols_;
    const std::string word = word_at(s, c);
    if (right) {
        if (word == "more" || word == "bottom") return key_aid(Aid::RollDown);
        return KeyResult::Ignored;
    }
    const int key = function_key_at(s, c);
    if (key >= 1 && key <= 24) return key_aid(function_key_aid(key));
    const char sign = scroll_sign_at(s, c);
    if (sign == '+') return key_aid(Aid::RollUp);
    if (sign == '-') return key_aid(Aid::RollDown);
    if (word == "more") return key_aid(Aid::RollUp);
    if (word == "bottom") return key_aid(Aid::RollDown);
    const std::string number = menu_choice_at(s, c);
    if (!number.empty()) return pointer_menu_choice(number);
    return KeyResult::Ignored;
}

// "The number ... is placed in the entry field and an emulated Enter key
// AID is sent". Which entry field is not said: the one the text cursor is
// in, or else the first one [design]. Its old contents are erased.
KeyResult Engine::pointer_menu_choice(const std::string& number) {
    int fi = field_index_at(cursor_);
    if (fi < 0 || fields_[size_t(fi)].bypass()) fi = next_input_field(-1, true);
    if (fi < 0) return KeyResult::Ignored;
    const int start = fields_[size_t(fi)].start;
    if (fields_[size_t(fi)].last_enterable() - start + 1 < int(number.size())) return KeyResult::Ignored;
    set_cursor(start);
    key_function(Key::EraseEof);
    for (char d : number) {
        const KeyResult r = key_data(uint8_t(0xF0 + (d - '0')));
        if (r != KeyResult::Accepted) return r;
    }
    if (locked_) return KeyResult::Accepted;   // an auto-enter field sent it already
    return key_aid(Aid::Enter);
}

} // namespace tn5250
} // namespace em34xx
