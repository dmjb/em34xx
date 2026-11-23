// SPDX-License-Identifier: GPL-3.0-or-later
// The controller's default processing of mouse button events (FR chapter
// 17): processing exceptions, error reset, hot spots, the mouse selection
// entry field, text-cursor positioning and the double-click Enter.
#include "tn5250_test_util.hpp"

#include <doctest/doctest.h>

using namespace tn5250_test;

namespace {

// A menu as an older application draws it (rows and columns 1-based in
// the stream): three choices, an entry field of 10 at row 20 column 7 with
// the cursor in it, and scrolling and command key text below.
Bytes menu_stream() {
    return cat({{0x04, 0x40},
                {0x04, 0x11, 0x00, 0x08},
                {0x11, 0x01, 0x1E}, eb("Sign-on Menu"),
                {0x11, 0x04, 0x06}, eb("1. User tasks"),
                {0x11, 0x05, 0x06}, eb("2. Office tasks"),
                {0x11, 0x06, 0x05}, eb("10. Communications"),
                {0x11, 0x06, 0x28}, eb("Rate 3.5"),
                {0x11, 0x12, 0x40}, eb("More..."),
                {0x11, 0x12, 0x02}, eb("Bottom"),
                {0x11, 0x13, 0x02}, eb("Selection or command    More: - +   Net -5"),
                {0x11, 0x14, 0x06, 0x1D, 0x40, 0x00, 0x24, 0x00, 0x0A},
                {0x11, 0x16, 0x02}, eb("F3=Exit   F4=Prompt   F12=Cancel"),
                {0x13, 0x14, 0x07},
                {0x04, 0x52, 0x00, 0x00}});
}

// Column (0-based) of `text` in a row.
int col_of(const Host& h, int row, const std::string& text) {
    const size_t p = h.row(row).find(text);
    REQUIRE(p != std::string::npos);
    return int(p);
}

KeyResult click(Host& h, int row, int col, PointerButton b = PointerButton::Left,
                PointerAction a = PointerAction::Press, bool shift = false) {
    PointerEvent ev;
    ev.button = b;
    ev.action = a;
    ev.shift = shift;
    ev.row = row;
    ev.col = col;
    return h.e.pointer_event(ev);
}

// Clicks `text` in `row` (at `offset` into it) and returns the AID sent,
// or 0 if no record went to the host.
int aid_for(int row, const std::string& text, int offset = 0, PointerButton b = PointerButton::Left) {
    Host h;
    h.write(menu_stream());
    h.out.clear();
    click(h, row, col_of(h, row, text) + offset, b);
    if (h.out.empty()) return 0;
    CHECK(h.e.indicators().keyboard_locked);
    return h.out.front().data.at(2);
}

} // namespace

TEST_CASE("tn5250 pointer: command key text is a hot spot") {
    CHECK(aid_for(21, "F3=Exit") == 0x33);
    CHECK(aid_for(21, "F3=Exit", 5) == 0x33);          // on its description
    CHECK(aid_for(21, "F4=Prompt", 8) == 0x34);
    CHECK(aid_for(21, "F12=Cancel") == 0x3C);
    CHECK(aid_for(21, "F12=Cancel", 9) == 0x3C);
    CHECK(aid_for(21, "F3=Exit", 8) == 0);             // the gap between two keys
    CHECK(aid_for(21, "F12=Cancel", 12) == 0);         // past the last one
}

TEST_CASE("tn5250 pointer: scrolling hot spots") {
    CHECK(aid_for(17, "More...") == 0xF5);                          // Roll Up
    CHECK(aid_for(17, "More...", 0, PointerButton::Right) == 0xF4); // Roll Down
    CHECK(aid_for(17, "Bottom", 3) == 0xF4);
    CHECK(aid_for(17, "Bottom", 3, PointerButton::Right) == 0xF4);
    CHECK(aid_for(18, "- +", 2) == 0xF5);
    CHECK(aid_for(18, "- +") == 0xF4);
    CHECK(aid_for(18, "More: - +") == 0xF5);
    // A hyphen inside a word and a sign on a number are text.
    CHECK(aid_for(0, "-on Menu") == 0);
    CHECK(aid_for(18, "-5") == 0);
    // The right button does nothing anywhere else.
    CHECK(aid_for(21, "F3=Exit", 0, PointerButton::Right) == 0);
    CHECK(aid_for(3, "1. User", 0, PointerButton::Right) == 0);
}

TEST_CASE("tn5250 pointer: a menu choice goes into the entry field and Enter is sent") {
    Host h;
    h.write(menu_stream());
    h.type("XYZ");                                   // replaced by the choice
    CHECK(click(h, 5, col_of(h, 5, "Communications") + 4) == KeyResult::Accepted);
    const Record r = h.take();
    CHECK(r.data.at(2) == 0xF1);
    // Cursor after the number in the entry field, then the field: SBA 20,7 "10".
    CHECK(r.data == Bytes{0x14, 0x09, 0xF1, 0x11, 0x14, 0x07, 0xF1, 0xF0});
    CHECK(h.e.indicators().keyboard_locked);

    // On the number itself; a decimal number is not a choice.
    Host h2;
    h2.write(menu_stream());
    CHECK(click(h2, 4, col_of(h2, 4, "2. Office")) == KeyResult::Accepted);
    CHECK(h2.take().data == Bytes{0x14, 0x08, 0xF1, 0x11, 0x14, 0x07, 0xF2});
    Host h3;
    h3.write(menu_stream());
    CHECK(click(h3, 5, col_of(h3, 5, "3.5")) == KeyResult::Accepted);
    CHECK(h3.out.empty());
    CHECK(h3.e.cursor_row() == 5);                   // only the text cursor moved
}

TEST_CASE("tn5250 pointer: a left press positions the text cursor; a double click there is Enter") {
    Host h;
    h.write(menu_stream());
    CHECK(click(h, 10, 40) == KeyResult::Accepted);
    CHECK(h.e.cursor_row() == 10);
    CHECK(h.e.cursor_col() == 40);
    CHECK(h.out.empty());
    CHECK(click(h, 10, 40, PointerButton::Left, PointerAction::Release) == KeyResult::Ignored);
    CHECK(click(h, 10, 40, PointerButton::Left, PointerAction::DoubleClick) == KeyResult::Accepted);
    const Record r = h.take();
    CHECK(r.data.at(0) == 11);
    CHECK(r.data.at(1) == 41);
    CHECK(r.data.at(2) == 0xF1);

    // In an entry field it positions too, and text there is not a hot spot.
    Host h2;
    h2.write(menu_stream());
    h2.type("F3=EXIT");
    CHECK(click(h2, 19, 8) == KeyResult::Accepted);
    CHECK(h2.out.empty());
    CHECK(h2.e.cursor_col() == 8);

    // A double click without a positioning press before it does nothing:
    // the cursor has moved since, or the press was a hot spot.
    Host h3;
    h3.write(menu_stream());
    click(h3, 10, 40);
    h3.e.key_function(Key::CursorRight);
    CHECK(click(h3, 10, 40, PointerButton::Left, PointerAction::DoubleClick) == KeyResult::Ignored);
    CHECK(click(h3, 10, 40, PointerButton::Middle) == KeyResult::Ignored);
    CHECK(h3.out.empty());
}

TEST_CASE("tn5250 pointer: processing exceptions") {
    // Keyboard locked (no unlock, no read).
    Host h;
    h.write(cat({{0x04, 0x40, 0x04, 0x11, 0x00, 0x00, 0x11, 0x16, 0x02}, eb("F3=Exit")}));
    CHECK(h.e.indicators().keyboard_locked);
    CHECK(click(h, 21, 1) == KeyResult::Ignored);
    // Unlocked, but the controller has no read pending.
    h.write({0x04, 0x11, 0x00, 0x08});
    CHECK_FALSE(h.e.indicators().keyboard_locked);
    CHECK(click(h, 21, 1) == KeyResult::Ignored);
    CHECK(h.e.cursor_row() != 21);
    CHECK(h.out.empty());

    // Shifted events (copy and paste) are not implemented.
    Host h2;
    h2.write(menu_stream());
    CHECK(click(h2, 21, 1, PointerButton::Left, PointerAction::Press, true) == KeyResult::Ignored);
    CHECK(h2.out.empty());
}

TEST_CASE("tn5250 pointer: in operator error state only a left press on the error line acts, as Reset") {
    Host h;
    h.write(menu_stream());
    h.e.key_function(Key::CursorUp);
    CHECK(h.e.key_data(0xC1) == KeyResult::Error);
    CHECK(h.row(23) == " 0005");
    CHECK(click(h, 21, 1) == KeyResult::Ignored);                       // F3=Exit: not now
    CHECK(click(h, 23, 30, PointerButton::Right) == KeyResult::Ignored);
    CHECK(h.e.indicators().error == ErrorState::PreHelp);
    CHECK(click(h, 23, 30) == KeyResult::Accepted);
    CHECK(h.e.indicators().error == ErrorState::None);
    CHECK(h.row(23).empty());
    CHECK(h.out.empty());
}

TEST_CASE("tn5250 pointer: mouse selection entry field returns its AID with the cursor at the pointer") {
    Host h;
    h.write(one_field(5, 10, 0x4000, 8, {0x8A70}));
    CHECK(click(h, 4, 13) == KeyResult::Accepted);
    const Record r = h.take();
    CHECK(r.data.at(0) == 5);
    CHECK(r.data.at(1) == 14);
    CHECK(r.data.at(2) == 0x70);
    CHECK(h.e.indicators().keyboard_locked);
}
