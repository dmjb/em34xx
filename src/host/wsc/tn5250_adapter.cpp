// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/wsc/tn5250_adapter.hpp"

namespace em34xx {
namespace wsc {

uint8_t indicator_bits(const tn5250::Indicators& ind) {
    // WDLC to indicators [doc PAI 2.4.16.1]: 80 message waiting, 08 insert
    // mode, 02 input inhibited.
    return uint8_t((ind.message_waiting ? 0x80 : 0) | (ind.insert_mode ? 0x08 : 0) |
                   (ind.input_inhibited ? 0x02 : 0));
}

void Tn5250DisplayAdapter::screen_snapshot(ScreenState& out) const {
    tn5250::ScreenSnapshot snap;
    session_.snapshot(snap);
    out.rows = unsigned(snap.rows);
    out.cols = unsigned(snap.cols);
    out.cells.resize(snap.cells.size());
    // The contents of a non-display field (a password) never go to the
    // device: the station shows nothing there anyway, and this keeps them
    // off the cable, out of the device's buffer and out of screenshots,
    // whatever the terminal makes of the attribute. The engine keeps the
    // real data for the reply to the host.
    for (size_t i = 0; i < snap.cells.size(); ++i) {
        const tn5250::Cell& c = snap.cells[i];
        out.cells[i] = !c.is_attribute() && tn5250::attr_nondisplay(c.attr) ? uint8_t(0x00) : c.ch;
    }
    out.cursor = unsigned(snap.cursor_row * snap.cols + snap.cursor_col);
    out.indicators = indicator_bits(snap.indicators);
    out.indicators_3477 = 0;
    out.cursor_blink = snap.indicators.cursor_blink;
    out.clicker = true;
}

void Tn5250DisplayAdapter::pointer_action(const PointerAction& a) {
    tn5250::PointerEvent ev;
    ev.button = a.button == PointerAction::Button::Left ? tn5250::PointerButton::Left
                : a.button == PointerAction::Button::Right ? tn5250::PointerButton::Right
                                                           : tn5250::PointerButton::Middle;
    ev.action = a.kind == PointerAction::Kind::Press ? tn5250::PointerAction::Press
                : a.kind == PointerAction::Kind::Release ? tn5250::PointerAction::Release
                                                         : tn5250::PointerAction::DoubleClick;
    ev.shift = a.shift;
    ev.row = int(a.row);
    ev.col = int(a.col);
    session_.pointer_event(ev);
}

void Tn5250DisplayAdapter::operator_action(const OperatorAction& a) {
    using K = OperatorAction::Kind;
    using tn5250::Key;
    switch (a.kind) {
    case K::Character:
        session_.key_data(a.ebcdic);
        return;
    case K::Aid:
        if (a.aid == uint8_t(tn5250::Aid::Help)) session_.key_function(Key::Help);   // error help in error state
        else session_.key_aid(tn5250::Aid(a.aid));
        return;
    case K::Attn:
        session_.key_function(Key::Attention);
        return;
    case K::SysReq:
        session_.key_function(Key::SysReq);
        return;
    case K::Function:
        break;
    case K::Overrun:
    case K::Unmapped:
        return;
    }
    Key k;
    switch (a.function) {
    case KeyFunction::FieldExit: k = Key::FieldExit; break;
    case KeyFunction::FieldPlus: k = Key::FieldPlus; break;
    case KeyFunction::FieldMinus: k = Key::FieldMinus; break;
    case KeyFunction::NewLine: k = Key::NewLine; break;
    case KeyFunction::Tab: k = Key::FieldAdvance; break;
    case KeyFunction::Backtab: k = Key::FieldBackspace; break;
    case KeyFunction::Backspace: k = Key::Backspace; break;
    case KeyFunction::CursorUp: k = Key::CursorUp; break;
    case KeyFunction::CursorDown: k = Key::CursorDown; break;
    case KeyFunction::CursorLeft: k = Key::CursorLeft; break;
    case KeyFunction::CursorRight: k = Key::CursorRight; break;
    case KeyFunction::Home: k = Key::Home; break;
    case KeyFunction::Insert: k = Key::Insert; break;
    case KeyFunction::Delete: k = Key::Delete; break;
    case KeyFunction::Dup: k = Key::Dup; break;
    case KeyFunction::EraseEof: k = Key::EraseEof; break;
    case KeyFunction::EraseInput: k = Key::EraseInput; break;
    case KeyFunction::Reset: k = Key::Reset; break;
    case KeyFunction::FieldMark: k = Key::FieldMark; break;
    default: return;   // Hex, Record/Play, Test, Set CP: not modelled by the engine
    }
    session_.key_function(k);
}

} // namespace wsc
} // namespace em34xx
