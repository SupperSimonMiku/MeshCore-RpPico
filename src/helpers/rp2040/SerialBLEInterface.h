#pragma once

#include "../BaseSerialInterface.h"
#include <BLE.h>

// NOTE (RP2040/Pico W port): the Arduino-Pico BLE stack only exposes
// BLESecurityNone / BLESecurityJustWorks -- there is no passkey/MITM API
// like Bluefruit's Security.setPIN(), so unlike the nRF52 and ESP32
// builds, pairing here can't be gated by BLE_PIN_CODE. See begin() below.
//
// TX power is also not exposed by this BLE library, so BLE_TX_POWER is
// accepted for source compatibility but has no effect here.
#ifndef BLE_TX_POWER
#define BLE_TX_POWER 0
#endif

class SerialBLEInterface : public BaseSerialInterface, public BLEServerCallbacks {
  BLEServiceUART uart;
  bool _isEnabled;
  bool _isDeviceConnected;
  unsigned long _last_health_check;
  unsigned long _last_retry_attempt;

  struct Frame {
    uint8_t len;
    uint8_t buf[MAX_FRAME_SIZE];
  };

  #define FRAME_QUEUE_SIZE  12

  uint8_t send_queue_len;
  Frame send_queue[FRAME_QUEUE_SIZE];

  uint8_t recv_queue_len;
  Frame recv_queue[FRAME_QUEUE_SIZE];

  void clearBuffers();
  void shiftSendQueueLeft();
  void shiftRecvQueueLeft();

public:
  // uart's internal tx/rx buffers must be able to hold one full MeshCore
  // frame (MAX_FRAME_SIZE) without triggering a mid-frame auto-flush,
  // which would split one frame across two BLE notifications.
  SerialBLEInterface() : uart(MAX_FRAME_SIZE + 16, MAX_FRAME_SIZE + 16) {
    _isEnabled = false;
    _isDeviceConnected = false;
    _last_health_check = 0;
    _last_retry_attempt = 0;
    send_queue_len = 0;
    recv_queue_len = 0;
  }

  /**
   * init the BLE interface. On return, if BLE.begin() succeeded, the
   * device is already advertising and discoverable -- no separate
   * enable() call is required (mirrors the nRF52/ESP32 ports, which start
   * advertising internally during their own begin()).
   * @param prefix   a prefix for the device name
   * @param name  IN/OUT - a name for the device (combined with prefix). If "@@MAC", is modified and returned
   * @param pin_code   UNUSED on RP2040 -- this stack has no passkey pairing API. Kept for call-site compatibility.
   */
  void begin(const char* prefix, char* name, uint32_t pin_code);

  void disconnect();
  void enable() override;
  void disable() override;
  bool isEnabled() const override { return _isEnabled; }
  bool isConnected() const override;
  bool isWriteBusy() const override;
  size_t writeFrame(const uint8_t src[], size_t len) override;
  size_t checkRecvFrame(uint8_t dest[]) override;

  // BLEServerCallbacks
  void onConnect(BLEServer* p) override;
  void onDisconnect(BLEServer* p) override;
};

#if BLE_DEBUG_LOGGING && ARDUINO
  #include <Arduino.h>
  #define BLE_DEBUG_PRINT(F, ...) Serial.printf("BLE: " F, ##__VA_ARGS__)
  #define BLE_DEBUG_PRINTLN(F, ...) Serial.printf("BLE: " F "\n", ##__VA_ARGS__)
#else
  #define BLE_DEBUG_PRINT(...) {}
  #define BLE_DEBUG_PRINTLN(...) {}
#endif
