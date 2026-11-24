// SPDX-License-Identifier: GPL-3.0-or-later
// IBM 3477 system hardware: interrupt controller, timer, keyboard interface,
// two-wire bus and setup EEPROM.
#include "bus/bus.hpp"
#include "devices/battery_ram.hpp"
#include "devices/i2c.hpp"
#include "devices/ibm3477/i2c.hpp"
#include "devices/ibm3477/keyboard_if.hpp"
#include "devices/ibm3477/system.hpp"

#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace em34xx;
using Src = Ibm3477Interrupts::Source;

namespace {

constexpr uint64_t kClock = 8000000;

struct Sys {
    Ibm3477KeyboardInterface* kbd;
    std::unique_ptr<Ibm3477System> sys;
    Sys() {
        auto k = std::make_unique<Ibm3477KeyboardInterface>(kClock, KeyboardType::Terminal122, 0xBF, 0xBF);
        kbd = k.get();
        sys = std::make_unique<Ibm3477System>(kClock, uint8_t(0x0C), std::move(k));
        sys->reset();
    }
    void w(uint32_t a, uint8_t v) { sys->mmio_write8(a - 0x20000, v); }
    uint8_t r(uint32_t a) { return sys->mmio_read8(a - 0x20000); }
    void run_us(uint64_t us) {
        for (uint64_t i = 0; i < us; ++i) sys->tick(kClock / 1000000);
    }
    // Wait for a received keyboard byte, as the firmware's ISR would.
    bool receive(uint8_t& byte, uint64_t max_us = 5000) {
        for (uint64_t i = 0; i < max_us; ++i) {
            if (r(0x20013) & 0x04) {
                byte = r(0x20012);
                w(0x20013, 0x03);
                w(0x20014, 0x00);
                w(0x20013, 0x02);
                return true;
            }
            sys->tick(kClock / 1000000);
        }
        return false;
    }
    // Send a byte and take the transmit-complete interrupt.
    void send(uint8_t b) {
        w(0x20013, 0x03);
        w(0x20014, 0x00);
        w(0x20012, b);
        w(0x20013, 0x02);
        CHECK((r(0x20013) & 0x02) != 0);   // busy while sending
        for (int i = 0; i < 5000 && (r(0x20013) & 0x02); ++i) sys->tick(8);
        CHECK((r(0x20013) & 0x02) == 0);
        CHECK((r(0x20013) & 0x04) == 0);   // nothing received yet
        CHECK(kbd->interrupt_pending());
        w(0x20013, 0x03);
        w(0x20014, 0xFF);
        CHECK_FALSE(kbd->interrupt_pending());
        w(0x20013, 0x02);
    }
};

bool odd_parity_bit(uint8_t b) {
    int n = 0;
    for (int i = 0; i < 8; ++i) n += (b >> i) & 1;
    return (n & 1) == 0;
}

std::string temp_path(const char* name) {
    return std::string("em34xx_test_") + name;
}

// Bit-banging master, as the firmware drives the bus.
struct Master {
    I2cBus& bus;
    bool scl = true, sda = true;
    void set(bool c, bool d) { scl = c; sda = d; bus.set_lines(c, d); }
    void start() { set(true, true); set(true, false); set(false, false); }
    void stop() { set(false, false); set(true, false); set(true, true); }
    bool write(uint8_t b) {
        for (int i = 7; i >= 0; --i) {
            const bool bit = (b >> i) & 1;
            set(false, bit);
            set(true, bit);
            set(false, bit);
        }
        set(false, true);
        set(true, true);
        const bool ack = !bus.sda_in();
        set(false, true);
        return ack;
    }
    uint8_t read(bool ack) {
        uint8_t v = 0;
        set(false, true);
        for (int i = 0; i < 8; ++i) {
            set(true, true);
            v = uint8_t((v << 1) | (bus.sda_in() ? 1 : 0));
            set(false, true);
        }
        set(false, !ack);
        set(true, !ack);
        set(false, !ack);
        set(false, true);
        return v;
    }
};

void eeprom_write(Master& m, uint8_t addr, const std::vector<uint8_t>& data) {
    m.start();
    REQUIRE(m.write(0xA0));
    REQUIRE(m.write(addr));
    for (uint8_t b : data) REQUIRE(m.write(b));
    m.stop();
}

std::vector<uint8_t> eeprom_read(Master& m, uint8_t addr, size_t n) {
    m.start();
    REQUIRE(m.write(0xA0));
    REQUIRE(m.write(addr));
    m.start();   // repeated START
    REQUIRE(m.write(0xA1));
    std::vector<uint8_t> v;
    for (size_t i = 0; i < n; ++i) v.push_back(m.read(i + 1 < n));
    m.stop();
    return v;
}

} // namespace

TEST_CASE("3477 interrupt vectors and priority") {
    Ibm3477Interrupts ic;
    CHECK_FALSE(ic.intr_asserted());

    // Latched sources need their mask bit.
    ic.raise(Src::Timer);
    CHECK(ic.requests() == 0x8000);
    CHECK_FALSE(ic.intr_asserted());
    ic.set_mask(0x807F);
    CHECK(ic.intr_asserted());
    CHECK(ic.acknowledge() == 0x06);

    // Timer has priority over the Twinax sources; level semantics: the
    // source stays pending after INTA until acknowledged (W1C).
    ic.raise(Src::Twinax0Event);
    ic.raise(Src::Twinax0Status);
    CHECK(ic.acknowledge() == 0x06);
    ic.acknowledge_bits(0x8000);
    CHECK(ic.acknowledge() == 0x07);
    ic.acknowledge_bits(0x0001);
    CHECK(ic.acknowledge() == 0x09);
    ic.acknowledge_bits(0x0002);
    CHECK_FALSE(ic.intr_asserted());

    // Channel 1 sources are masked by the normal mask 807F.
    ic.raise(Src::Twinax1Event);
    ic.raise(Src::Twinax1Status);
    CHECK_FALSE(ic.intr_asserted());
    ic.set_mask(0xFFFF);
    CHECK(ic.acknowledge() == 0x08);
    ic.lower(Src::Twinax1Event);
    CHECK(ic.acknowledge() == 0x0A);
    ic.lower(Src::Twinax1Status);

    // Display (always enabled) before keyboard (20007 bit 0).
    ic.raise(Src::Keyboard);
    CHECK_FALSE(ic.intr_asserted());
    ic.set_keyboard_enabled(true);
    CHECK(ic.acknowledge() == 0x0E);
    ic.raise(Src::Display);
    CHECK(ic.acknowledge() == 0x0C);
    ic.lower(Src::Display);
    CHECK(ic.acknowledge() == 0x0E);
    ic.lower(Src::Keyboard);
    CHECK_FALSE(ic.intr_asserted());
}

TEST_CASE("interrupt vectors of the 3486 family's three-channel layout") {
    Ibm3477Interrupts ic;
    ic.set_keyboard_enabled(true);
    ic.raise(Src::Keyboard);
    CHECK(ic.acknowledge() == 0x0E);
    CHECK(ic.vector(Src::Display) == 0x0C);
    CHECK(ic.vector(Src::Twinax1Status) == 0x0A);
    ic.set_three_channel_layout();
    CHECK(ic.acknowledge() == 0x0F);
    CHECK(ic.vector(Src::Timer) == 0x06);
    CHECK(ic.vector(Src::Twinax1Event) == 0x08);
    CHECK(ic.vector(Src::Twinax0Status) == 0x0A);
    CHECK(ic.vector(Src::Display) == 0x0D);
    ic.reset();   // the layout is wiring, not state
    CHECK(ic.vector(Src::Keyboard) == 0x0F);

    // The third channel's requests (2000D) and mask (2000F).
    ic.request2(0x01);
    CHECK_FALSE(ic.intr_asserted());
    ic.set_mask2(0x7F);
    CHECK(ic.acknowledge() == 0x09);
    ic.acknowledge_bits2(0x01);
    ic.request2(0x08);
    CHECK(ic.acknowledge() == 0x0C);
    ic.acknowledge_bits2(0x08);
    CHECK_FALSE(ic.intr_asserted());
}

TEST_CASE("3477 interrupt registers in the gate array") {
    Sys s;
    s.w(0x20004, 0x7F);
    s.w(0x20005, 0x80);
    CHECK(s.r(0x20004) == 0x7F);
    CHECK(s.r(0x20005) == 0x80);
    s.sys->interrupts().request(0x8003);
    CHECK(s.r(0x20002) == 0x03);
    CHECK(s.r(0x20003) == 0x80);
    s.w(0x20003, 0x80);   // write 1 to clear
    CHECK(s.r(0x20003) == 0x00);
    CHECK(s.r(0x20002) == 0x03);
    s.w(0x20002, 0xFF);
    s.w(0x20003, 0xFF);
    CHECK(s.sys->interrupts().requests() == 0);
    // Twinax channel events: acknowledging "command queued" (02) drops the
    // queue load's "end of message" (08) with it; any other acknowledge
    // clears just its own bits.
    s.sys->interrupts().request(0x0A0A);
    s.w(0x20002, 0x02);
    CHECK(s.r(0x20002) == 0x00);
    CHECK(s.r(0x20003) == 0x0A);
    s.w(0x20003, 0x08);
    CHECK(s.r(0x20003) == 0x02);
    s.w(0x20003, 0x02);
    s.sys->interrupts().request(0x000C);
    s.w(0x20002, 0x04);   // activate done: the end of the Activate Write stays
    CHECK(s.r(0x20002) == 0x08);
    s.w(0x20002, 0x08);
    CHECK(s.sys->interrupts().requests() == 0);
    s.w(0x20007, 0x01);
    CHECK(s.sys->interrupts().keyboard_enabled());
    s.w(0x20007, 0x00);
    CHECK_FALSE(s.sys->interrupts().keyboard_enabled());
    CHECK(s.r(0x2000A) == 0x0C);
}

TEST_CASE("3477 timer runs at 50 Hz") {
    Sys s;
    CHECK(s.sys->timer_period_cycles() == kClock / 50);
    s.w(0x20005, 0x80);
    int ticks = 0;
    // One emulated second in 1000-cycle steps, acknowledging each tick.
    for (uint64_t c = 0; c < kClock; c += 1000) {
        s.sys->tick(1000);
        if (s.sys->interrupts().intr_asserted()) {
            CHECK(s.sys->interrupts().acknowledge() == 0x06);
            s.w(0x20003, 0x80);
            ++ticks;
        }
    }
    CHECK(ticks == 50);
}

TEST_CASE("3477 alarm tone and speaker gate reach the audio sink") {
    struct Recorder : AudioSink {
        std::vector<std::string> events;
        void tone(bool on, double hz) override { events.push_back(std::string("tone ") + (on ? "on " : "off ") + std::to_string(int(hz))); }
        void speaker(bool on) override { events.push_back(on ? "speaker on" : "speaker off"); }
        void click() override { events.push_back("click"); }
    } rec;
    Sys s;
    s.sys->set_audio(&rec);
    s.w(0x20010, 0x85);   // tone on
    s.w(0x20010, 0x86);   // setting change only: no event
    s.w(0x20015, 0x02);   // speaker gate on
    s.w(0x20015, 0x00);
    s.w(0x20010, 0x80);
    s.sys->reset();       // tone still on: reset turns it off
    const std::vector<std::string> want = {"tone on 0", "speaker on", "speaker off", "tone off 0"};
    CHECK(rec.events == want);
}

TEST_CASE("3477 straps follow the display subtype") {
    CHECK(ibm3477_default_straps(DisplaySubtype::Green) == 0x0C);
    CHECK(ibm3477_default_straps(DisplaySubtype::Amber) == 0x0C);
    CHECK(ibm3477_default_straps(DisplaySubtype::PaperWhite) == 0x0E);
    CHECK(ibm3477_default_straps(DisplaySubtype::Color) == 0x0C);
}

TEST_CASE("3477 keyboard interface: power-on, reset and ID handshake") {
    Sys s;
    s.w(0x20007, 0x01);
    // Clock held low (01): the keyboard is inhibited and nothing arrives.
    s.w(0x20013, 0x01);
    s.run_us(3000);
    CHECK((s.r(0x20013) & 0x04) == 0);

    s.w(0x20013, 0x02);
    uint8_t b = 0;
    REQUIRE(s.receive(b));
    CHECK(b == 0xAA);
    REQUIRE(s.receive(b));
    CHECK(b == 0xBF);
    REQUIRE(s.receive(b));
    CHECK(b == 0xBF);
    CHECK_FALSE(s.receive(b));

    // Reset command: FA, AA and the ID bytes; each byte raises vector 0E.
    s.send(0xFF);
    std::vector<uint8_t> got;
    for (int i = 0; i < 4; ++i) {
        for (int t = 0; t < 5000 && !(s.r(0x20013) & 0x04); ++t) s.sys->tick(8);
        REQUIRE(s.sys->interrupts().intr_asserted());
        CHECK(s.sys->interrupts().acknowledge() == 0x0E);
        const uint8_t st = s.r(0x20013);
        const uint8_t d = s.r(0x20012);
        CHECK(bool(st & 1) == odd_parity_bit(d));
        got.push_back(d);
        s.w(0x20013, 0x03);
        s.w(0x20014, 0x00);
        CHECK_FALSE(s.sys->interrupts().intr_asserted());
        s.w(0x20013, 0x02);
    }
    CHECK(got == std::vector<uint8_t>{0xFA, 0xAA, 0xBF, 0xBF});

    // Interrupt is gated by 20007 bit 0.
    s.w(0x20007, 0x00);
    s.send(0xEE);
    CHECK_FALSE(s.sys->interrupts().intr_asserted());
    s.run_us(3000);
    CHECK((s.r(0x20013) & 0x04) != 0);
    CHECK_FALSE(s.sys->interrupts().intr_asserted());
    CHECK(s.r(0x20012) == 0xEE);
}

TEST_CASE("3477 keyboard interface: key-mode configuration sequence is acknowledged") {
    Sys s;
    s.w(0x20013, 0x02);
    uint8_t b;
    while (s.receive(b)) {}
    const std::vector<uint8_t> seq = {0xF9, 0xFB, 0x61, 0x6A, 0xF3, 0x2C, 0xFC, 0x12, 0x59, 0xF4};
    for (uint8_t c : seq) {
        s.send(c);
        REQUIRE(s.receive(b));
        CHECK(b == 0xFA);
    }
    CHECK(s.kbd->keyboard().key_mode(0x61) == Set3Keyboard::Typematic);
    CHECK(s.kbd->keyboard().key_mode(0x12) == Set3Keyboard::MakeBreak);
    CHECK(s.kbd->keyboard().key_mode(0x1C) == Set3Keyboard::MakeOnly);
    CHECK(s.kbd->keyboard().typematic() == 0x2C);
}

TEST_CASE("3477 keyboard interface: manual-clock wrap test") {
    Sys s;
    s.w(0x20013, 0x01);
    s.w(0x20014, 0xFF);
    s.w(0x20007, 0x01);
    for (uint8_t pattern : {uint8_t(0xAB), uint8_t(0x55)}) {
        s.w(0x20012, pattern);
        for (int i = 0; i < 11; ++i) {
            CHECK_FALSE(s.sys->interrupts().intr_asserted());
            s.w(0x20013, 0x01);
            s.w(0x20013, 0x03);
        }
        CHECK(s.sys->interrupts().intr_asserted());
        CHECK(s.sys->interrupts().acknowledge() == 0x0E);
        const uint8_t st = s.r(0x20013);
        CHECK((st & 0x04) != 0);
        CHECK(bool(st & 1) == odd_parity_bit(pattern));
        CHECK(s.r(0x20012) == pattern);
        s.w(0x20014, 0xFF);
        CHECK_FALSE(s.sys->interrupts().intr_asserted());
    }
    // The keyboard itself saw nothing.
    CHECK(s.kbd->bytes_sent() == 0);
}

TEST_CASE("I2C EEPROM byte write, page wrap and random read") {
    I2cBus bus;
    I2cEeprom e("eeprom");
    std::string err;
    REQUIRE(e.open("", err));
    e.attach(bus, 0xA0);
    I2cLogDevice mon("monitor");
    bus.attach(0x8C >> 1, &mon);
    Master m{bus};

    CHECK(eeprom_read(m, 0x10, 2) == std::vector<uint8_t>{0xFF, 0xFF});
    eeprom_write(m, 0x1B, {0x12});
    eeprom_write(m, 0x29, {0x34, 0x56});
    CHECK(e.byte(0x1B) == 0x12);
    CHECK(eeprom_read(m, 0x29, 2) == std::vector<uint8_t>{0x34, 0x56});
    CHECK(eeprom_read(m, 0x1B, 1) == std::vector<uint8_t>{0x12});

    // Page write wraps within the 8-byte page.
    eeprom_write(m, 0x46, {1, 2, 3});
    CHECK(e.byte(0x46) == 1);
    CHECK(e.byte(0x47) == 2);
    CHECK(e.byte(0x40) == 3);

    // Absent address: no acknowledge.
    m.start();
    CHECK_FALSE(m.write(0xB0));
    m.stop();

    // Write-only device at 8C acknowledges and logs.
    m.start();
    CHECK(m.write(0x8C));
    CHECK(m.write(0x02));
    CHECK(m.write(0x5A));
    m.stop();
    CHECK(mon.last_message() == std::vector<uint8_t>{0x02, 0x5A});
    CHECK(e.dirty());   // ephemeral: changes never saved
}

TEST_CASE("I2C EEPROM persistence") {
    const std::string path = temp_path("eeprom.bin");
    std::remove(path.c_str());
    {
        I2cBus bus;
        I2cEeprom e("eeprom");
        std::string err;
        REQUIRE(e.open(path, err));   // missing file: blank
        CHECK(e.byte(0) == 0xFF);
        e.attach(bus, 0xA0);
        Master m{bus};
        eeprom_write(m, 0x05, {0xA5});
        CHECK_FALSE(e.dirty());       // saved at STOP
        std::ifstream f(path, std::ios::binary);
        CHECK(f.good());
    }
    {
        I2cEeprom e("eeprom");
        std::string err;
        REQUIRE(e.open(path, err));
        CHECK(e.byte(0x05) == 0xA5);
        CHECK(e.byte(0x06) == 0xFF);
        // A byte edited from the monitor is saved at once; out of range is
        // refused.
        REQUIRE(e.nv_set(0x30, 0xAA, err));
        CHECK(e.byte(0x30) == 0xAA);
        CHECK_FALSE(e.nv_set(0x100, 0x00, err));
        // Changes made outside a bus transaction are saved on destruction.
        e.set_byte(0x07, 0x77);
    }
    {
        // An image of the wrong size is moved aside, not refused: the
        // EEPROM starts blank and keeps its file name.
        I2cEeprom e("eeprom", 512, 16);
        std::string err;
        REQUIRE(e.open(path, err));
        CHECK_FALSE(e.ephemeral());
        CHECK(e.byte(0x05) == 0xFF);
        CHECK_FALSE(std::ifstream(path, std::ios::binary).good());
        CHECK(std::ifstream(path + ".bad", std::ios::binary).good());
    }
    CHECK_FALSE(std::ifstream(path + ".tmp", std::ios::binary).good());
    std::remove(path.c_str());
    std::remove((path + ".bad").c_str());
}

TEST_CASE("I2C EEPROM ephemeral contents are discarded") {
    const std::string path = temp_path("eeprom_save.bin");
    std::remove(path.c_str());
    {
        I2cBus bus;
        I2cEeprom e("eeprom");
        std::string err;
        REQUIRE(e.open("", err));
        CHECK(e.ephemeral());
        e.attach(bus, 0xA0);
        Master m{bus};
        eeprom_write(m, 0x00, {0x11});
        CHECK(e.nv_status().find("ephemeral") != std::string::npos);
        CHECK_FALSE(e.nv_save("", err));       // nowhere to save
        REQUIRE(e.nv_save(path, err));         // explicit save works
    }
    I2cEeprom e2("eeprom");
    std::string err;
    REQUIRE(e2.open("", err));
    CHECK(e2.byte(0) == 0xFF);                 // a new ephemeral EEPROM is blank
    REQUIRE(e2.open(path, err));
    CHECK(e2.byte(0) == 0x11);
    std::remove(path.c_str());
}

TEST_CASE("Battery-backed RAM keeps its contents across power cycles") {
    const std::string path = temp_path("nvram.bin");
    std::remove(path.c_str());
    {
        Bus bus{20};
        bus.map_ram("ram", 0, 0x20000);
        BatteryBackedRam nv("nvram", bus, 0, 0x20000);
        std::string err;
        REQUIRE(nv.open(path, err));   // missing file: RAM starts cleared
        CHECK(bus.peek8(0x1A0) == 0x00);
        bus.poke8(0x1A0, 0x5A);
        bus.poke8(0x1FFFF, 0xA5);
    }   // power-off: the image is written
    {
        Bus bus{20};
        bus.map_ram("ram", 0, 0x20000);
        BatteryBackedRam nv("nvram", bus, 0, 0x20000);
        std::string err;
        REQUIRE(nv.open(path, err));
        CHECK(bus.peek8(0x1A0) == 0x5A);
        CHECK(bus.peek8(0x1FFFF) == 0xA5);
    }
    {
        // A wrong-sized image is moved aside, not refused: the RAM starts
        // cleared and a fresh image is written at power-off.
        Bus bus{20};
        bus.map_ram("ram", 0, 0x20000);
        BatteryBackedRam nv("nvram", bus, 0, 0x10000);
        std::string err;
        REQUIRE(nv.open(path, err));
        CHECK_FALSE(nv.volatile_ram());
        CHECK(bus.peek8(0x1A0) == 0x00);
        CHECK_FALSE(std::ifstream(path, std::ios::binary).good());
        bus.poke8(0x1A0, 0x3C);
    }
    {
        std::ifstream bad(path + ".bad", std::ios::binary | std::ios::ate);
        REQUIRE(bad.good());
        CHECK(bad.tellg() == std::streampos(0x20000));
        std::ifstream fresh(path, std::ios::binary | std::ios::ate);
        REQUIRE(fresh.good());
        CHECK(fresh.tellg() == std::streampos(0x10000));
    }
    CHECK_FALSE(std::ifstream(path + ".tmp", std::ios::binary).good());
    std::remove(path.c_str());
    std::remove((path + ".bad").c_str());
}
