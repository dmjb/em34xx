// SPDX-License-Identifier: GPL-3.0-or-later
#include "host/wsc/device_commands.hpp"

namespace em34xx {
namespace wsc {

namespace {

void push_addr(std::vector<uint8_t>& b, unsigned addr) {
    b.push_back(uint8_t(addr >> 8));
    b.push_back(uint8_t(addr));
}

} // namespace

QueueLoad& QueueLoad::load_address(unsigned addr) {
    bytes_.push_back(cmd::LoadAddressCounter);
    push_addr(bytes_, addr);
    return *this;
}

QueueLoad& QueueLoad::load_reference(unsigned addr) {
    bytes_.push_back(cmd::LoadReferenceCounter);
    push_addr(bytes_, addr);
    return *this;
}

QueueLoad& QueueLoad::load_cursor(unsigned addr) {
    bytes_.push_back(cmd::LoadCursor);
    push_addr(bytes_, addr);
    return *this;
}

QueueLoad& QueueLoad::clear() {
    bytes_.push_back(cmd::Clear);
    return *this;
}

QueueLoad& QueueLoad::write_data_load_cursor(const uint8_t* data, size_t n) {
    bytes_.push_back(cmd::WriteDataLoadCursor);
    bytes_.push_back(uint8_t(n));
    bytes_.insert(bytes_.end(), data, data + n);
    return *this;
}

QueueLoad& QueueLoad::write_indicators(uint8_t bits) {
    bytes_.push_back(cmd::WriteIndicators);
    bytes_.push_back(bits);
    return *this;
}

QueueLoad& QueueLoad::write_control(uint8_t byte1, int byte2, int byte3) {
    bytes_.push_back(cmd::WriteControlData);
    if (byte2 >= 0 && byte3 >= 0) {
        bytes_.push_back(uint8_t(byte1 | wcd::Byte2Follows));
        bytes_.push_back(uint8_t(byte2 | wcd::B2Byte3Follows));
        bytes_.push_back(uint8_t(byte3));
    } else if (byte2 >= 0) {
        bytes_.push_back(uint8_t(byte1 | wcd::Byte2Follows));
        bytes_.push_back(uint8_t(byte2));
    } else {
        bytes_.push_back(uint8_t(byte1 & ~wcd::Byte2Follows));
    }
    return *this;
}

QueueLoad& QueueLoad::write_control_indicators(uint8_t bits) {
    bytes_.push_back(cmd::WriteControlIndicators);
    bytes_.push_back(uint8_t(bits & ~0x01));   // bit 01 would announce a second byte
    return *this;
}

QueueLoad& QueueLoad::set_mode(uint8_t mode) {
    bytes_.push_back(cmd::SetMode);
    bytes_.push_back(mode);
    return *this;
}

QueueLoad& QueueLoad::write_data() {
    bytes_.push_back(cmd::WriteData);
    return *this;
}

QueueLoad& QueueLoad::read_device_id(uint8_t command) {
    bytes_.push_back(command);
    return *this;
}

QueueLoad& QueueLoad::read_model_data() {
    bytes_.push_back(cmd::ReadModelData);
    return *this;
}

QueueLoad& QueueLoad::read_info(uint8_t parameter) {
    bytes_.push_back(cmd::ReadInfo);
    bytes_.push_back(parameter);
    return *this;
}

QueueLoad& QueueLoad::select_record(uint8_t record) {
    bytes_.push_back(cmd::Extended);
    bytes_.push_back(cmd::ExtSelectRecord);
    bytes_.push_back(record);
    return *this;
}

QueueLoad& QueueLoad::read_record(unsigned bytes) {
    load_address(0);
    load_reference(bytes - 1);
    bytes_.push_back(cmd::ReadRecord);
    return *this;
}

QueueLoad& QueueLoad::raw(uint8_t byte) {
    bytes_.push_back(byte);
    return *this;
}

QueueLoad& QueueLoad::end_of_queue() {
    bytes_.push_back(cmd::EndOfQueue);
    return *this;
}

TwinaxMessage make_message(const std::vector<uint8_t>& bytes, uint8_t station) {
    TwinaxMessage msg;
    msg.reserve(bytes.size());
    for (size_t i = 0; i < bytes.size(); ++i) {
        const bool last = i + 1 == bytes.size();
        msg.push_back(TwinaxFrame{bytes[i], uint8_t((last && bytes.size() > 1) ? 7 : station)});
    }
    return msg;
}

TwinaxMessage make_poll(uint8_t station, bool reset_lpe) {
    const uint8_t c = uint8_t(cmd::Poll | cmd::PollAck | (reset_lpe ? cmd::PollResetLpe : 0));
    return {TwinaxFrame{c, station}};
}

} // namespace wsc
} // namespace em34xx
