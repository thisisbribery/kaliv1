// KALI SMASH: Kali-specific hardware bits on top of libDaisy's DaisyPatchSM.
// Pin map confirmed on hardware 2026-11-14 (see hardware/PINMAP.md).
#pragma once
#include "daisy_patch_sm.h"
#include "dev/oled_ssd130x.h"

namespace kali
{
using namespace daisy;
using patch_sm::DaisyPatchSM;

// 128x32 SSD1306 on I2C1 (0x3C). libDaisy's 128x32 init matches stock Kali, but its Update() starts
// each page at column 32; stock starts at column 0.
class OledDriver : public SSD130xDriver<128, 32, SSD130xI2CTransport>
{
  public:
    // Skips the I2C transfer when nothing on screen changed: less bus traffic, less chance of it
    // leaking into the audio as buzz.
    void Update()
    {
        uint32_t h = 2166136261u;
        for(size_t i = 0; i < sizeof(buffer_); i++)
            h = (h ^ buffer_[i]) * 16777619u;
        if(h == lastHash_)
            return;
        lastHash_ = h;
        for(uint8_t i = 0; i < 4; i++)
        {
            transport_.SendCommand(0xB0 + i);
            transport_.SendCommand(0x00);
            transport_.SendCommand(0x10);
            transport_.SendData(&buffer_[128 * i], 128);
        }
    }

  private:
    uint32_t lastHash_ = 0;
};
using Oled = OledDisplay<OledDriver>;

// LFO 1-4: TI DAC7554 on SPI2 (SCK D10, MOSI D9, hardware NSS D1). 12-bit, 0 = -V, 4095 = +V.
class Dac7554
{
  public:
    void Init()
    {
        SpiHandle::Config c;
        c.periph          = SpiHandle::Config::Peripheral::SPI_2;
        c.mode            = SpiHandle::Config::Mode::MASTER;
        c.direction       = SpiHandle::Config::Direction::TWO_LINES_TX_ONLY;
        c.datasize        = 8;
        c.clock_polarity  = SpiHandle::Config::ClockPolarity::HIGH;
        c.clock_phase     = SpiHandle::Config::ClockPhase::ONE_EDGE;
        c.nss             = SpiHandle::Config::NSS::HARD_OUTPUT;
        c.baud_prescaler  = SpiHandle::Config::BaudPrescaler::PS_8;
        c.pin_config.sclk = DaisyPatchSM::D10;
        c.pin_config.mosi = DaisyPatchSM::D9;
        c.pin_config.miso = Pin(PORTX, 0);
        c.pin_config.nss  = DaisyPatchSM::D1;
        spi_.Init(c);
    }
    void Write(int ch, uint16_t code)
    {
        if(code > 4095)
            code = 4095;
        uint16_t cmd    = (2u << 14) | ((uint16_t)ch << 12) | code;
        uint8_t  buf[2] = {(uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xff)};
        spi_.BlockingTransmit(buf, 2, 5);
    }

  private:
    SpiHandle spi_;
};

// Panel names for the Patch SM controls
namespace pin
{
// Main knobs left to right = CV_1 .. CV_8
static constexpr int kKnob[8] = {patch_sm::CV_1, patch_sm::CV_2, patch_sm::CV_3, patch_sm::CV_4,
                                 patch_sm::CV_5, patch_sm::CV_6, patch_sm::CV_7, patch_sm::CV_8};
static constexpr int kMiniLeft  = patch_sm::ADC_9;  // LFO ADJ
static constexpr int kMiniRight = patch_sm::ADC_10; // DELAY ADJ
} // namespace pin

} // namespace kali
