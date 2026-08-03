#include <Arduino.h>
#include "target.h"
#include <helpers/ArduinoHelpers.h>

PicoWBoard board;

// GP18/GP16/GP19 are the RP2040's default SPI0 pins, which is what your
// wiring (LORA_SCK=18, LORA_MISO=16, LORA_MOSI=19) uses. The "SPI" object
// on the Arduino-Pico core is SPI0. Use "SPI1" here instead if you ever
// rewire onto GP10/11/12/14.
static SPIClassRP2040 &spi = SPI;

// SX1276 constructor order is (NSS, DIO0, RESET, DIO1, spi) -- note this is
// different from the SX1262 variant, which uses (NSS, DIO1, RESET, BUSY, spi).
RADIO_CLASS radio = new Module(P_LORA_NSS, P_LORA_DIO_0, P_LORA_RESET, P_LORA_DIO_1, spi);

WRAPPER_CLASS radio_driver(radio, board);

VolatileRTCClock fallback_clock;
AutoDiscoverRTCClock rtc_clock(fallback_clock);
SensorManager sensors;

bool radio_init() {
  rtc_clock.begin(Wire);

  return radio.std_init(&spi);
}

mesh::LocalIdentity radio_new_identity() {
  RadioNoiseListener rng(radio);
  return mesh::LocalIdentity(&rng);  // create new random identity
}