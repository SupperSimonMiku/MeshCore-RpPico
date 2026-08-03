#include "SerialBLEInterface.h"
#include <stdio.h>
#include <string.h>
#include <pico/unique_id.h>

// RX drain buffer size for overflow protection
#define BLE_RX_DRAIN_BUF_SIZE      32

static SerialBLEInterface* instance = nullptr;

void SerialBLEInterface::onConnect(BLEServer* p) {
  (void)p;
  BLE_DEBUG_PRINTLN("SerialBLEInterface: connected");
  _isDeviceConnected = true;
  clearBuffers();
}

void SerialBLEInterface::onDisconnect(BLEServer* p) {
  (void)p;
  BLE_DEBUG_PRINTLN("SerialBLEInterface: disconnected");
  _isDeviceConnected = false;
  clearBuffers();

  // This BLE stack does not appear to auto-resume advertising after a
  // disconnect (unlike Bluefruit's restartOnDisconnect(true)), so kick it
  // back on explicitly if we're still supposed to be enabled/discoverable.
  if (_isEnabled) {
    BLE.startAdvertising(true);  // must match begin() -- see comment there
  }
}

void SerialBLEInterface::begin(const char* prefix, char* name, uint32_t pin_code) {
  instance = this;
  (void)pin_code;  // not supported by this BLE stack -- see header note

  char dev_name[32 + 16];
  if (strcmp(name, "@@MAC") == 0) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    // last 3 bytes of the RP2040 flash's unique ID, same source variant.cpp
    // uses to build the mesh MAC, so the BLE name and mesh identity line up
    sprintf(name, "%02X%02X%02X", id.id[5], id.id[6], id.id[7]);
  }
  sprintf(dev_name, "%s%s", prefix, name);

  // No passkey pairing is available on this stack (see header). Using
  // BLESecurityNone avoids getting stuck in a pairing flow the app can't
  // complete. If you want an encrypted (but un-authenticated) link instead,
  // switch this to BLESecurityJustWorks.
  BLE.setSecurity(BLESecurityJustWorks);

  // Per the Arduino-Pico BLE docs, the correct order is: begin() first,
  // THEN register the service, THEN start advertising. An earlier version
  // of this file added the service before begin() -- that was wrong.
  BLE.begin(dev_name);

  BLE.server()->setCallbacks(this);
  BLE.server()->addService(&uart);

  uart.setAutoflush(50);

  // IMPORTANT: startAdvertising() takes a bool controlling whether the
  // 128-bit service UUID is included in the 31-byte advertising packet.
  // This MUST be true. The companion apps scan using a service-UUID
  // filter at the OS/BLE-stack level (e.g. CoreBluetooth's
  // scanForPeripherals(withServices:) or the Android equivalent) --
  // that filtering happens before any app code sees a result, so if the
  // UUID isn't in the advertising packet, the device is silently
  // invisible to a fresh scan no matter what the advertised name says.
  // (Confirmed: with this false, generic tools like LightBlue -- which
  // scan without a UUID filter and match on name -- could see the
  // device, but the actual companion apps could not, except via a
  // "retrieve already-connected peripherals" fallback that only kicked
  // in while some other app happened to hold a GATT connection open.)
  //
  // A full 128-bit UUID structure (18 bytes) plus flags (3 bytes)
  // leaves little of the 31-byte advertising budget for the device
  // name, so the on-air name may be truncated -- that's fine. The name
  // isn't needed for discovery/matching once the UUID gets a client to
  // connect; the full name is still available afterwards via the
  // standard GAP Device Name (set by BLE.begin(dev_name) below) or by
  // reading it off our own service after connecting.
  //
  // This also fixes the previous bug where nothing called enable()/
  // startAdvertising() at all after begin(), so the device never
  // actually advertised.
  _isEnabled = true;
  BLE.startAdvertising(true);
  BLE_DEBUG_PRINTLN("SerialBLEInterface: begin() done, advertising as '%s'", dev_name);
}

void SerialBLEInterface::clearBuffers() {
  send_queue_len = 0;
  recv_queue_len = 0;
  _last_retry_attempt = 0;
}

void SerialBLEInterface::shiftSendQueueLeft() {
  if (send_queue_len > 0) {
    send_queue_len--;
    for (uint8_t i = 0; i < send_queue_len; i++) {
      send_queue[i] = send_queue[i + 1];
    }
  }
}

void SerialBLEInterface::shiftRecvQueueLeft() {
  if (recv_queue_len > 0) {
    recv_queue_len--;
    for (uint8_t i = 0; i < recv_queue_len; i++) {
      recv_queue[i] = recv_queue[i + 1];
    }
  }
}

void SerialBLEInterface::enable() {
  // NOTE: begin() already enables + starts advertising on success, so this
  // is mainly here for main.cpp/mesh-loop call sites that (on other
  // platforms) toggle the radio on/off at runtime. Deliberately no
  // "if (_isEnabled) return;" early-out -- calling startAdvertising()
  // again when already advertising is harmless, and this keeps enable()
  // usable as a safe "make sure we're on" call.
  _isEnabled = true;
  clearBuffers();
  _last_health_check = millis();

  BLE.startAdvertising(true);  // must match begin() -- see comment there
}

void SerialBLEInterface::disconnect() {
  // NOTE: this BLE library does not expose a public "kick the current
  // client" API (BLEServer keeps its connection handle private). Stopping
  // advertising is the closest available action; the link itself will only
  // drop when the central disconnects or the connection times out.
  BLE_DEBUG_PRINTLN("SerialBLEInterface: disconnect() requested, but this BLE stack has no forced-disconnect API -- ignoring");
}

void SerialBLEInterface::disable() {
  _isEnabled = false;
  BLE_DEBUG_PRINTLN("SerialBLEInterface: disable");

  BLE.stopAdvertising();
  _last_health_check = 0;
}

size_t SerialBLEInterface::writeFrame(const uint8_t src[], size_t len) {
  if (len > MAX_FRAME_SIZE) {
    BLE_DEBUG_PRINTLN("writeFrame(), frame too big, len=%u", (unsigned)len);
    return 0;
  }

  bool connected = isConnected();
  if (connected && len > 0) {
    if (send_queue_len >= FRAME_QUEUE_SIZE) {
      BLE_DEBUG_PRINTLN("writeFrame(), send_queue is full!");
      return 0;
    }

    send_queue[send_queue_len].len = len;
    memcpy(send_queue[send_queue_len].buf, src, len);
    send_queue_len++;

    return len;
  }
  return 0;
}

size_t SerialBLEInterface::checkRecvFrame(uint8_t dest[]) {
  // -- outgoing --
  if (send_queue_len > 0) {
    if (!isConnected()) {
      BLE_DEBUG_PRINTLN("writeBytes: connection invalid, clearing send queue");
      send_queue_len = 0;
    } else {
      Frame& frame_to_send = send_queue[0];

      // Write the whole frame into uart's internal tx buffer, then force an
      // immediate flush so it goes out as a single BLE notification --
      // matching the one-frame-per-packet assumption the companion app
      // protocol relies on. (See constructor: uart's buffer is sized to
      // hold one full frame so this write() loop can't trigger a premature
      // auto-flush partway through.)
      uart.write(frame_to_send.buf, frame_to_send.len);
      uart.flush();

      BLE_DEBUG_PRINTLN("writeBytes: sz=%u, hdr=%u", (unsigned)frame_to_send.len, (unsigned)frame_to_send.buf[0]);
      shiftSendQueueLeft();
    }
  }

  // -- incoming --
  if (recv_queue_len < FRAME_QUEUE_SIZE) {
    int avail = uart.available();
    if (avail > 0) {
      if (avail > MAX_FRAME_SIZE) {
        BLE_DEBUG_PRINTLN("checkRecvFrame: WARN: BLE RX overflow, avail=%d, draining all", avail);
        uint8_t drain_buf[BLE_RX_DRAIN_BUF_SIZE];
        while (uart.available() > 0) {
          int chunk = uart.available() > BLE_RX_DRAIN_BUF_SIZE ? BLE_RX_DRAIN_BUF_SIZE : uart.available();
          for (int i = 0; i < chunk; i++) uart.read();
          (void)drain_buf;
        }
      } else {
        Frame& f = recv_queue[recv_queue_len];
        f.len = avail;
        for (int i = 0; i < avail; i++) {
          f.buf[i] = (uint8_t)uart.read();
        }
        recv_queue_len++;
      }
    }
  }

  if (recv_queue_len > 0) {
    size_t len = recv_queue[0].len;
    memcpy(dest, recv_queue[0].buf, len);

    BLE_DEBUG_PRINTLN("readBytes: sz=%u, hdr=%u", (unsigned)len, (unsigned)dest[0]);

    shiftRecvQueueLeft();
    return len;
  }

  return 0;
}

bool SerialBLEInterface::isConnected() const {
  return _isDeviceConnected;
}

bool SerialBLEInterface::isWriteBusy() const {
  return send_queue_len >= (FRAME_QUEUE_SIZE * 2 / 3);
}
