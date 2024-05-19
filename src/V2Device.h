// © Kay Sievers <kay@versioduo.com>, 2020-2022
// SPDX-License-Identifier: Apache-2.0

#pragma once

#define ARDUINOJSON_USE_DOUBLE 0
#include <ArduinoJson.h>
#include <V2Base.h>
#include <V2LED.h>
#include <V2Link.h>
#include <V2MIDI.h>

class V2Device : public V2MIDI::Port {
public:
  // Device metadata stored in a global variable.
  struct Metadata {
    // Reverse-domain, unique device identifier (e.g. com.example.frobnicator).
    const char* id;

    // The version will always be presented to the user as a simple decimal number.
    const uint32_t version;

    // The fully-qualified board name.
    const char* board;

    // JSON object, it can be read from the offline firmware image. It needs to
    // be an embedded array not a pointer, to be able to retrieve its location
    // and export the offset to the end of the file.
    const char json[];
  };

  struct {
    // Few words, title line describing the function.
    const char* description{};

    // Human readable text, also used as USB strings.
    const char* vendor{};
    const char* product{};

    // Link to a website, including protocol prefix.
    const char* home{};
  } metadata;

  // Help texts, paragraphs are separated by newline.
  struct {
    const char* device{};
    const char* configuration{};
  } help;

  struct {
    // Link to firmware the image updates, including the protocol prefix. It expects an
    // 'index.json' file at the location.
    const char* download{};

    // Link to the WebMIDI configuration site. The link is advertized over WebUSB.
    const char* configure{};

    // A specific hardware revision number encoded with GPIO pins connected to ground.
    uint8_t revision{};
  } system;

  // Custom USB IDs, initialized with the board specified values.
  struct {
    // Custom USB device name.
    const char* name{};

    uint16_t vid{USB_VID};
    uint16_t pid{USB_PID};

    // Number of MIDI ports / virtual cables to access children devices.
    struct {
      // The default number of ports
      uint8_t standard{1};

      // The number of ports to enable when 'rebootWithPorts' is called
      // to gain access to the children devices.
      uint8_t access{};
      bool    enableAccess{};

      // The currently active number of ports.
      uint8_t current{1};
    } ports;

    V2MIDI::USBDevice midi{};
  } usb;

  V2Link* link{};

  V2MIDI::SerialDevice* serial{};

  // Built-in LED.
  V2LED::Basic led;

  // Local device-specific configuration which will be read and written to the EEPROM.
  struct {
    uint16_t version; // A different version calls handleEEPROM() to possibly convert from.
    uint16_t size;
    void*    data;
  } configuration{};

  // The maximum system exclusive message size. It needs to carry at least the firmware
  // update packet of 8k bytes -> base64 encoded -> wrapped in a JSON object -> ~12kb.
  //
  // Default USB MIDI port 0.
  constexpr V2Device() : V2Device(16 * 1024){};
  constexpr V2Device(uint32_t sysexSize) : Port(0, sysexSize), led(PIN_LED_ONBOARD, &_ledTimer), _ledTimer(3, 1000) {}

  // Read the configuration from the EEPROM, initialize the bootup data which
  // might be carried over to the next reboot.
  void begin();

  void reset();
  void loop();

  // Return if there is pending work, e.g. queued messages.
  bool idle();

  // Wait for interrupts, goes into sleep mode IDLE. The system tick will wake it
  // up at least once every millisecond.
  void sleep() {
    V2Base::Power::sleep();
  }

  // Write the configuration to the EEPROM.
  void writeConfiguration();

protected:
  // Called after reading the configuration from the EEPROM, before USB is initialized.
  virtual void handleInit() {}

  virtual void handleReset() {}

  virtual void handleLoop() {}

  // Called when updateConfiguration() is called. Parses the config and writes it to
  // the EEPROM.
  virtual void importConfiguration(JsonObject json) {}

  // The human readable device properties, e.g. name, vendor, product, description.
  virtual void exportMetadata(JsonObject json) {}

  // A list of external links to web applications using the device.
  virtual void exportLinks(JsonArray json) {}

  // The machine-readable device properties, e.g. state, statistics, firmware update URL.
  virtual void exportSystem(JsonObject json) {}

  // List of configuration objects pointing to data in the configuration and providing
  // metadata to create sections in the configuration editor.
  virtual void exportSettings(JsonArray json) {}

  // The device configuration. A single JSON record to edit/backup/restore.
  virtual void exportConfiguration(JsonObject json) {}

  // The Notes and controllers the device listens to.
  virtual void exportInput(JsonObject json) {}

  // The notes and controllers the device sends out.
  virtual void exportOutput(JsonObject json) {}

  // Read the binary configuration from an different/older version.
  virtual void handleEEPROM(uint16_t version, const void* data, uint32_t size) {}

private:
  struct EEPROM {
    const struct Header {
      uint32_t magic{0x7ed63a8b};
      uint32_t size{sizeof(EEPROM)};
    } header;

    // The device-specific part.
    struct {
      uint16_t magic;
      uint16_t version;
      uint32_t size;
    } local{};

    struct {
      // The custom name of the USB device.
      char name[32]{};

      // Custom USB VID/PID.
      uint16_t vid{};
      uint16_t pid{};

      // The number of MIDI ports to create.
      uint8_t ports{};
      uint8_t padding{};
    } usb;
  } _eeprom;

  struct {
    uint32_t id;
  } _boot{};

  struct {
    char hash[41];
  } _firmware{};

  V2Base::Timer::Periodic _ledTimer;

  void sendReply(V2MIDI::Transport* transport);
  void sendFirmwareStatus(V2MIDI::Transport* transport, const char* status);
  void handleSystemExclusive(V2MIDI::Transport* transport, const uint8_t* buffer, uint32_t len) override;
  bool readEEPROM(bool dryrun = false);
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
      "\0{\"com.versioduo.firmware\":{"                                                                                \
      "\"id\":\"" _id "\","                                                                                            \
      "\"version\":" #_version ","                                                                                     \
      "\"board\":\"" _board "\"}}"                                                                                     \
    }                                                                                                                  \
  }
