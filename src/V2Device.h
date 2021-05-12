// © Kay Sievers <kay@vrfy.org>, 2020-2021
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <ArduinoJson.h>
#include <V2MIDI.h>
#include <V2Memory.h>
#include <V2Power.h>

class V2Device : public V2MIDI::Port {
public:
  // Device metadata stored in a global variable.
  struct Metadata {
    // Reverse-domain, unique device identifier (e.g. com.example.frobnicator).
    const char *id;

    // The version will always be presented to the user as a simple decimal number.
    const uint32_t version;

    // The fully-qualified Arduino board name (fqbn).
    const char *board;

    // JSON object, it can be read from the offline firmware image. It needs to
    // be an embedded array not a pointer, to be able to retrieve its location
    // and export the offset to the end of the file.
    const char json[];
  };

  struct {
    // Human readable text, also used as USB strings.
    const char *vendor{};
    const char *product{};

    // Brief text, describing the device
    const char *description{};

    // Link to a website, including protocol prefix.
    const char *home{};
  } metadata;

  struct {
    // Custom USB device name.
    const char *name{};

    // Number of MIDI ports / virtual cables to access children devices.
    struct Ports {
      uint8_t configured{1};
      uint8_t announce{1};
      uint8_t current{1};
      uint8_t reboot{};
    } ports;

    // Link to firmware the image updates, including the protocol prefix. It expects an
    // 'index.json' file at the location.
    const char *download{};
  } system;

  // Custom USB IDs, initialized with the board specified values.
  struct {
    uint16_t vid{USB_VID};
    uint16_t pid{USB_PID};
    V2MIDI::USBDevice midi{};
  } usb;

  // Local device-specific configuration which will be read and written to the EEPROM.
  struct {
    uint32_t magic;
    uint16_t size;
    void *data;
  } configuration{};

  // 12k maximum system exclusive message size. The firmware update packet is 8k bytes,
  // base64 encoded, wrapped in JSON.
  static constexpr uint32_t sysex_max_size{12 * 1024};

  // Default port 0.
  V2Device() : Port(0, sysex_max_size) {}

  // Read the configuration from the EEPROM, initialize the bootup data which
  // might be carried over to the next reboot.
  void begin();

  void loop();

  // Return if there is pending work, e.g. queued messages.
  bool idle();

  // Wait for interrupts, goes into sleep mode IDLE. The system tick will wake it
  // up at least once every millisecond.
  void sleep() {
    V2Power::sleep();
  }

  // Write the configuration to the EEPROM.
  void writeConfiguration();

protected:
  // Called after reading the configuration from the EEPROM, before USB is initialized.
  virtual void handleInit() {}

  virtual void handleLoop() {}

  // Read JSON to update the configuration.
  virtual void importConfiguration(JsonObject json) {}

  // Export configuration as JSON.
  virtual void exportMetadata(JsonObject json) {}
  virtual void exportSystem(JsonObject json) {}
  virtual void exportInput(JsonObject json) {}
  virtual void exportOutput(JsonObject json) {}
  virtual void exportConfiguration(JsonObject json) {}

private:
  struct Configuration {
    struct Header {
      uint32_t magic{0x7ed63a89};
      uint32_t size{sizeof(_configuration)};
    } header;

    // The custom name of the USB device.
    char name[32]{};

    // The number of MIDI ports to create.
    uint8_t ports{1};

    // The device-specific part.
    struct {
      uint32_t magic;
      uint32_t size;
    } local{};
  } _configuration;

  struct {
    uint32_t id;
  } _boot{};

  struct {
    char hash[41];
  } _firmware{};

  void sendReply(V2MIDI::Transport *transport);
  void sendFirmwareStatus(V2MIDI::Transport *transport, const char *status);
  void handleSystemExclusive(V2MIDI::Transport *transport, const uint8_t *buffer, uint32_t len) override;
  void readEEPROM();
};

// Global variable, set with V2DEVICE_METADATA()
extern const V2Device::Metadata V2DeviceMetadata;

// Store the image metadata in a JSON record which is located at the very end
// of the firmware image, with a leading and trailing NUL character. The updater
// can read it and verify that the update file matches the board information.
// The "metadata" section requires explicit support from the linker script to
// be effective.
#define V2DEVICE_METADATA(_id, _version, _board)                                                                       \
  const V2Device::Metadata V2DeviceMetadata __attribute__((used)) __attribute__((section(".metadata"))) {              \
    _id, _version, _board, {                                                                                           \
      "\0{\"interface\":\"com.versioduo.firmware\","                                                                   \
      "\"id\":\"" _id "\","                                                                                            \
      "\"version\":" #_version ","                                                                                     \
      "\"board\":\"" _board "\"}"                                                                                      \
    }                                                                                                                  \
  }
