// SPDX-License-Identifier: GPL-3.0-or-later
// 5250 device commands as a workstation controller sends them, and the
// queue-load builder. Command bytes are `device << 5 | code` with the
// TwinaxFrame data byte holding PAI frame bit 7 in bit 7 (see
// docs/twinax-bridge.md). Sources: 5250 Product Attachment Information,
// Oct 1989, ch. 2 and 16 [doc].
#pragma once

#include "host/twinax_link.hpp"

#include <cstdint>
#include <vector>

namespace em34xx {
namespace wsc {

namespace cmd {
constexpr uint8_t ActivateRead = 0x00;
constexpr uint8_t ActivateWrite = 0x01;
constexpr uint8_t Reset = 0x02;
constexpr uint8_t WriteControlData = 0x05;
constexpr uint8_t LoadReferenceCounter = 0x07;
constexpr uint8_t ReadInfo = 0x0B;
constexpr uint8_t ReadDeviceId = 0x0C;
constexpr uint8_t Poll = 0x10;
constexpr uint8_t PollAck = 0x20;        // modifier: acknowledge the last response
constexpr uint8_t PollResetLpe = 0x40;   // modifier: reset line parity error
constexpr uint8_t WriteDataLoadCursor = 0x11;
constexpr uint8_t Clear = 0x12;
constexpr uint8_t SetMode = 0x13;
constexpr uint8_t LoadAddressCounter = 0x15;
constexpr uint8_t LoadCursor = 0x17;
constexpr uint8_t ReadRegisters = 0x1C;
constexpr uint8_t WriteData = 0x1E;

constexpr uint8_t DevKeyboard = 0x20;
constexpr uint8_t DevIndicators = 0x40;
constexpr uint8_t DevModel = 0x80;       // feature format, only without 3477 device addressing

constexpr uint8_t EndOfQueue = 0x62;                       // Reset to device 011
constexpr uint8_t ReadKeyboardId = DevKeyboard | ReadDeviceId;   // 2C
constexpr uint8_t ReadModelId = DevModel | ReadDeviceId;         // 8C
constexpr uint8_t ReadModelData = 0x88;                          // Read Data to model
constexpr uint8_t WriteIndicators = DevIndicators | WriteDataLoadCursor;  // 51
constexpr uint8_t WriteControlIndicators = DevIndicators | WriteControlData;  // 45
constexpr uint8_t ReadInfoVpd = 0x80;    // Read Info parameter
// 3486/3487 [obs, docs/twinax-bridge.md "Pointing device"].
constexpr uint8_t ReadInfoNative = 0xC0;    // Read Info parameter: the unit's own VPD
constexpr uint8_t Extended = 0x09;          // 09 sub [parameter]
constexpr uint8_t ExtSelectRecord = 0x09;   // record that device-101 reads address
constexpr uint8_t RecordPointer = 0x03;
constexpr uint8_t ReadRecord = 0xBA;        // read between the counters, device 101
} // namespace cmd

// Poll response status byte [doc PAI fig. 23/24].
namespace status {
constexpr uint8_t Busy = 0x80;
constexpr uint8_t LineParityError = 0x40;
constexpr uint8_t OutstandingStatus = 0x10;
constexpr uint8_t ExceptionMask = 0x0E;
constexpr uint8_t Level = 0x01;

constexpr uint8_t ExcNone = 0x00;
constexpr uint8_t ExcNullAttribute = 0x02;
constexpr uint8_t ExcInvalidActivate = 0x04;
constexpr uint8_t ExcInvalidCommand = 0x08;
constexpr uint8_t ExcOverrun = 0x0A;
constexpr uint8_t ExcInvalidRegister = 0x0C;
constexpr uint8_t ExcPowerOn = 0x0E;

inline uint8_t exception(uint8_t s) { return uint8_t(s & ExceptionMask); }
} // namespace status

// Write Control Data bits. Byte 1 [doc PAI]; byte 2 as the 3477 implements
// it [obs, private device spec; 40h and 02h are undocumented 3477 bits].
namespace wcd {
constexpr uint8_t CursorInhibit = 0x80;
constexpr uint8_t Byte2Follows = 0x40;
constexpr uint8_t BlinkCursor = 0x20;
constexpr uint8_t ReverseScreen = 0x08;
constexpr uint8_t ResetException = 0x04;
constexpr uint8_t ClickerOff = 0x02;
constexpr uint8_t Alarm = 0x01;

constexpr uint8_t B2Byte3Follows = 0x80;       // 3486/3487, after Read Info C0 [obs]
constexpr uint8_t B2DeviceAddressing = 0x40;   // 3477: E0h addresses "Both"
constexpr uint8_t B2Wide = 0x08;               // 27x132
constexpr uint8_t B2Queue64 = 0x02;            // 3477: 64-byte command queue
constexpr uint8_t B2MessageLine = 0x01;
constexpr uint8_t B3PointerEvents = 0x10;      // 3486/3487: report button events [obs]
} // namespace wcd

// A queue load: command and data bytes for one message. A load that ends
// with a preactivate command (Write Data, a read) is not followed by EOQ:
// the preactivate ends it.
class QueueLoad {
public:
    QueueLoad& load_address(unsigned addr);
    QueueLoad& load_reference(unsigned addr);
    QueueLoad& load_cursor(unsigned addr);
    QueueLoad& clear();
    // Write Data and Load Cursor with an explicit count frame (always sent,
    // so that data bytes 01..0E are never read as a count).
    QueueLoad& write_data_load_cursor(const uint8_t* data, size_t n);
    QueueLoad& write_indicators(uint8_t bits);
    // byte2 < 0: one byte; byte3 >= 0: three bytes (3486/3487).
    QueueLoad& write_control(uint8_t byte1, int byte2 = -1, int byte3 = -1);
    QueueLoad& write_control_indicators(uint8_t bits);
    QueueLoad& set_mode(uint8_t mode);
    QueueLoad& write_data();                  // preactivate: arms Activate Write
    QueueLoad& read_device_id(uint8_t command = cmd::ReadDeviceId);  // preactivate
    QueueLoad& read_model_data();             // preactivate
    QueueLoad& read_info(uint8_t parameter = cmd::ReadInfoVpd);   // preactivate
    QueueLoad& select_record(uint8_t record); // 3486/3487: 09 09 n
    QueueLoad& read_record(unsigned bytes);   // preactivate: the first `bytes` of the selected record
    QueueLoad& raw(uint8_t byte);
    QueueLoad& end_of_queue();

    const std::vector<uint8_t>& bytes() const { return bytes_; }
    size_t size() const { return bytes_.size(); }
    bool empty() const { return bytes_.empty(); }

private:
    std::vector<uint8_t> bytes_;
};

// Frames for one controller -> station message: the first frame carries the
// station address, later ones too, and the last one address 7 (EOM). A
// single-frame message carries the station address [doc PAI p.30-40].
TwinaxMessage make_message(const std::vector<uint8_t>& bytes, uint8_t station);
TwinaxMessage make_poll(uint8_t station, bool reset_lpe);

// Every queue load the controller sends fits the device's command queue:
// 16 bytes, or 64 on a 3477 after WCD byte 2 bit 02h.
constexpr size_t kQueueBytes = 16;
constexpr size_t kQueueBytes3477 = 64;

} // namespace wsc
} // namespace em34xx
