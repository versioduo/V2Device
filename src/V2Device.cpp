// © Kay Sievers <kay@vrfy.org>, 2020-2021
// SPDX-License-Identifier: Apache-2.0

#include <Arduino.h>
#include <V2Cryptography.h>

#include "V2Device.h"

// This is only initialized after a cold startup when the memory is undefined.
// A reset/reboot will not overwrite the data; it is retained across reset/reboot
// cycles.
// The "noinit" section needs to be outside of the "bss" section; it requires
// explicit support from the linker script to be effective.
static class BootData {
public:
  BootData() {
    if (_magic == 0x8f734e41)
      return;

    clear();
    _magic = 0x8f734e41;
  }

  void clear() {
    n_ports = 0;
  }

  // The number of MIDI ports to export to the host
  uint8_t n_ports;

private:
  uint32_t _magic;
} bootData __attribute__((section(".noinit")));

void V2Device::readEEPROM() {
  struct Configuration *eeprom = (struct Configuration *)V2Memory::EEPROM::getStart();

  // Check our magic, all bytes are 0xff after chip erase.
  if (eeprom->header.magic != _configuration.header.magic)
    return;

  if (eeprom->header.size <= sizeof(struct Configuration::Header))
    return;

  if (eeprom->header.size > sizeof(_configuration))
    return;

  memcpy(&_configuration, eeprom, eeprom->header.size);

  if (_configuration.name[0] != '\0')
    system.name = _configuration.name;

  if (_configuration.ports > 1)
    system.ports.configured = _configuration.ports;

  // Device-specific section.
  if (!configuration.data)
    return;

  if (eeprom->local.magic != configuration.magic)
    return;

  if (eeprom->local.size == 0)
    return;

  if (eeprom->local.size > configuration.size)
    return;

  memcpy(configuration.data, (const void *)(V2Memory::EEPROM::getStart() + eeprom->header.size), eeprom->local.size);
}

void V2Device::begin() {
  if (V2Memory::Flash::UserPage::update()) {
    // Reboot to enable the new settings.
    delay(100);
    V2Memory::Firmware::reboot();
  }

  _boot.id = V2Cryptography::Random::read();

  // Do not block in GetAll(), it takes ~80ms.
  V2Memory::Firmware::calculateHash(V2Memory::Firmware::getStart(), V2Memory::Firmware::getSize(), _firmware.hash);

  // Read a possible config from the previous boot cycle.
  if (bootData.n_ports > 1)
    system.ports.reboot = bootData.n_ports;

  bootData.clear();

  // The larger descriptor is needed to carry the data for more than 3 MIDI ports.
  {
    static uint8_t descriptor[1024];
    USBDevice.setConfigurationBuffer(descriptor, sizeof(descriptor));
  }

  // USB uses two two-digit BCD numbers; version 1 will be shown as 0.01, version 815 as 8.15.
  {
    uint16_t version = V2DeviceMetadata.version;
    uint16_t bcd     = 0;
    for (uint8_t shift = 0; shift < 16; shift += 4) {
      bcd += (version % 10) << shift;
      version /= 10;
    }
    USBDevice.setDeviceVersion(bcd);
  }

  readEEPROM();
  handleInit();

  // Set USB device name, the default is provided by the board package, the metadata
  // provides a product name, a custom name might be stored in the EEPROM.
  if (system.name)
    USBDevice.setProductDescriptor(system.name);

  else if (metadata.product)
    USBDevice.setProductDescriptor(metadata.product);

  // Set USB MIDI ports.
  {
    uint8_t ports = 1;
    if (system.ports.reboot > 1)
      ports = system.ports.reboot;

    else if (system.ports.configured > 1)
      ports = system.ports.configured;

    if (ports > 1) {
      system.ports.current = ports;
      usb.midi.setPorts(ports);
    }

    // Operating systems/services/apps get confused if the number
    // of ports changes between device connections; some hang, some
    // don't probe the device again and ignore the new number of ports.
    //
    // To work around it, let the USB ID depend on the number of ports.
    USBDevice.setID(usb.vid, usb.pid + ports - 1);
  }

  usb.midi.begin();

  // Sleep mode IDLE, wait for interrupts.
  V2Power::setSleepMode(V2Power::Mode::Idle);
}

void V2Device::loop() {
  sendSystemExclusive();
  handleLoop();
}

// Reply with message to indicate that we are ready for the next packet.
void V2Device::sendFirmwareStatus(V2MIDI::Transport *transport, const char *status) {
  uint8_t *reply = getSystemExclusiveBuffer();
  uint32_t len   = 0;

  // 0x7d == SysEx research/private ID
  reply[len++] = (uint8_t)V2MIDI::Packet::Status::SystemExclusive;
  reply[len++] = 0x7d;
  len += sprintf((char *)reply + len, R"({"com.versioduo.device":{"firmware":{"status":")");
  len += sprintf((char *)reply + len, status);
  len += sprintf((char *)reply + len, R"("}}})");
  reply[len++] = (uint8_t)V2MIDI::Packet::Status::SystemExclusiveEnd;
  setSystemExclusive(transport, len);
}

static int8_t utf8Codepoint(const uint8_t *utf8, uint32_t *codepointp) {
  uint32_t codepoint;
  int8_t len;

  if (utf8[0] < 0x80)
    len = 1;
  else if ((utf8[0] & 0xe0) == 0xc0)
    len = 2;
  else if ((utf8[0] & 0xf0) == 0xe0)
    len = 3;
  else if ((utf8[0] & 0xf8) == 0xf0)
    len = 4;
  else if ((utf8[0] & 0xfc) == 0xf8)
    len = 5;
  else if ((utf8[0] & 0xfe) == 0xfc)
    len = 6;
  else
    return -1;

  switch (len) {
    case 1:
      codepoint = utf8[0];
      break;

    case 2:
      codepoint = utf8[0] & 0x1f;
      break;

    case 3:
      codepoint = utf8[0] & 0x0f;
      break;

    case 4:
      codepoint = utf8[0] & 0x07;
      break;

    case 5:
      codepoint = utf8[0] & 0x03;
      break;

    case 6:
      codepoint = utf8[0] & 0x01;
      break;
  }

  for (int8_t i = 1; i < len; i++) {
    if ((utf8[i] & 0xc0) != 0x80)
      return -1;

    codepoint <<= 6;
    codepoint |= utf8[i] & 0x3f;
  }

  *codepointp = codepoint;
  return len;
}

static void readSerial(char *serial) {
  const uint8_t *bytes[4] = {(uint8_t *)0x008061FC,
                             (uint8_t *)0x00806010,
                             (uint8_t *)0x00806014,
                             (uint8_t *)0x00806018};
  sprintf(serial,
          "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
          bytes[0][3],
          bytes[0][2],
          bytes[0][1],
          bytes[0][0],
          bytes[1][3],
          bytes[1][2],
          bytes[1][1],
          bytes[1][0],
          bytes[2][3],
          bytes[2][2],
          bytes[2][1],
          bytes[2][0],
          bytes[3][3],
          bytes[3][2],
          bytes[3][1],
          bytes[3][0]);
}

// Escape unicode to fit into a 7 bit byte stream.
static uint32_t escapeJSON(const uint8_t *json_buffer, uint32_t json_len, uint8_t *buffer, uint32_t size) {
  uint32_t buffer_len = 0;

  for (uint32_t i = 0; i < json_len; i++) {
    if (json_buffer[i] > 0x7f) {
      uint32_t codepoint;
      uint8_t len = utf8Codepoint(json_buffer + i, &codepoint);
      if (len < 0)
        continue;

      // Advance the additional UTF8 characters for this codepoint.
      i += len - 1;

      if (codepoint < 0xffff) {
        if (buffer_len + 7 > size)
          return 0;

        buffer_len += sprintf((char *)buffer + buffer_len, "\\u%04x", codepoint);

      } else {
        if (buffer_len + 13 > size)
          return 0;

        codepoint -= 0x10000;
        uint16_t surrogate1 = (codepoint >> 10) + 0xd800;
        uint16_t surrogate2 = (codepoint & 0x3ff) + 0xdc00;
        buffer_len += sprintf((char *)buffer + buffer_len, "\\u%04x\\u%04x", surrogate1, surrogate2);
      }

    } else {
      if (buffer_len >= size)
        return 0;

      buffer[buffer_len++] = json_buffer[i];
    }
  }

  return buffer_len;
}

static void addBootloaderMetadata(JsonObject meta) {
  // The end of the bootloader contains an array of four offsets/pointers.
  const uint32_t *info = (uint32_t *)V2Memory::Firmware::getStart() - 4;

  // The first entry is the location of our metadata.
  const char *metadata = (const char *)info[0];

  StaticJsonDocument<2 * 1024> json;
  if (deserializeJson(json, metadata))
    return;

  JsonObject bootloader = json["com.versioduo.bootloader"];
  if (!bootloader)
    return;

  if (!bootloader["board"])
    return;

  meta["board"] = bootloader["board"];
}

// Send the current data as a SystemExclusive, JSON message.
void V2Device::sendReply(V2MIDI::Transport *transport) {
  uint8_t *reply = getSystemExclusiveBuffer();
  uint32_t len   = 0;

  // 0x7d == SysEx research/private ID
  reply[len++] = (uint8_t)V2MIDI::Packet::Status::SystemExclusive;
  reply[len++] = 0x7d;

  StaticJsonDocument<24 * 1024> json;
  JsonObject json_device = json.createNestedObject("com.versioduo.device");

  {
    JsonObject json_meta = json_device.createNestedObject("metadata");
    if (metadata.vendor)
      json_meta["vendor"] = metadata.vendor;

    if (metadata.product)
      json_meta["product"] = metadata.product;

    if (metadata.description)
      json_meta["description"] = metadata.description;

    if (metadata.home)
      json_meta["home"] = metadata.home;

    char serial[33];
    readSerial(serial);
    json_meta["serial"] = serial;

    json_meta["version"] = V2DeviceMetadata.version;
    exportMetadata(json_meta);
  }

  {
    JsonObject json_system = json_device.createNestedObject("system");
    if (system.name)
      json_system["name"] = system.name;

    addBootloaderMetadata(json_system);

    {
      JsonObject json_usb = json_system.createNestedObject("usb");
      json_usb["vid"]     = usb.vid;
      json_usb["pid"]     = usb.pid;
    }

    if (system.ports.announce > 0) {
      JsonObject json_ports    = json_system.createNestedObject("ports");
      json_ports["configured"] = system.ports.configured;
      json_ports["announce"]   = system.ports.announce;
      json_ports["current"]    = system.ports.current;
    }

    {
      JsonObject json_firmware = json_system.createNestedObject("firmware");
      if (system.download)
        json_firmware["download"] = system.download;
      json_firmware["id"]    = V2DeviceMetadata.id;
      json_firmware["board"] = V2DeviceMetadata.board;
      json_firmware["hash"]  = _firmware.hash;
      json_firmware["start"] = V2Memory::Firmware::getStart();
      json_firmware["size"]  = V2Memory::Firmware::getSize();
    }

    {
      JsonObject json_ram = json_system.createNestedObject("ram");
      json_ram["size"]    = V2Memory::RAM::getSize();
      json_ram["free"]    = V2Memory::RAM::getFree();
    }

    {
      JsonObject json_flash = json_system.createNestedObject("flash");
      json_flash["size"]    = V2Memory::Flash::getSize();
    }

    {
      JsonObject json_eeprom = json_system.createNestedObject("eeprom");
      json_eeprom["size"]    = V2Memory::EEPROM::getSize();
    }

    {
      JsonObject json_boot = json_system.createNestedObject("boot");
      json_boot["uptime"]  = (uint32_t)(millis() / 1000);
      json_boot["id"]      = _boot.id;
    }

    {
      JsonObject in              = json_system.createNestedObject("input");
      in["note"]                 = _statistics.input.note;
      in["noteOff"]              = _statistics.input.note_off;
      in["aftertouch"]           = _statistics.input.aftertouch;
      in["control"]              = _statistics.input.control;
      in["program"]              = _statistics.input.program;
      in["aftertouchChannel"]    = _statistics.input.aftertouch_channel;
      in["pitchbend"]            = _statistics.input.pitchbend;
      JsonObject in_system       = in.createNestedObject("system");
      in_system["exclusive"]     = _statistics.input.system.exclusive;
      in_system["reset"]         = _statistics.input.system.reset;
      JsonObject in_system_clock = in_system.createNestedObject("clock");
      in_system_clock["tick"]    = _statistics.input.system.clock.tick;
    }

    {
      JsonObject out              = json_system.createNestedObject("output");
      out["note"]                 = _statistics.output.note;
      out["noteOff"]              = _statistics.output.note_off;
      out["aftertouch"]           = _statistics.output.aftertouch;
      out["control"]              = _statistics.output.control;
      out["program"]              = _statistics.output.program;
      out["aftertouchChannel"]    = _statistics.output.aftertouch_channel;
      out["pitchbend"]            = _statistics.output.pitchbend;
      JsonObject out_system       = out.createNestedObject("system");
      out_system["exclusive"]     = _statistics.output.system.exclusive;
      out_system["reset"]         = _statistics.output.system.reset;
      JsonObject out_system_clock = out_system.createNestedObject("clock");
      out_system_clock["tick"]    = _statistics.output.system.clock.tick;
    }

    exportSystem(json_system);
  }

  JsonObject input = json_device.createNestedObject("input");
  exportInput(input);

  JsonObject output = json_device.createNestedObject("output");
  exportOutput(output);

  {
    JsonObject config = json_device.createNestedObject("configuration");
    config["#name"]   = "The device name (USB product string)";
    config["name"]    = _configuration.name;

    if (system.ports.announce > 0) {
      config["#ports"] = "The number of MIDI ports to create";
      config["ports"]  = _configuration.ports;
    }

    exportConfiguration(config);
  }

  {
    uint8_t json_buffer[sysex_max_size];
    uint32_t json_len = serializeJson(json, (char *)json_buffer, sysex_max_size);
    len += escapeJSON(json_buffer, json_len, reply + len, sysex_max_size - len);
  }

  reply[len++] = (uint8_t)V2MIDI::Packet::Status::SystemExclusiveEnd;
  setSystemExclusive(transport, len);
}

// Handle a SystemExclusive, JSON request from the host.
void V2Device::handleSystemExclusive(V2MIDI::Transport *transport, const uint8_t *buffer, uint32_t len) {
  if (len < 24)
    return;

  // 0x7d == SysEx prototype/research/private ID
  if (buffer[1] != 0x7d)
    return;

  // Handle only JSON messages.
  if (buffer[2] != '{' || buffer[len - 2] != '}')
    return;

  // Read incoming message.
  StaticJsonDocument<16 * 1024> json;
  if (deserializeJson(json, buffer + 2, len - 1))
    return;

  // Only handle requests for our interface.
  JsonObject json_device = json["com.versioduo.device"];
  if (!json_device)
    return;

  if (json_device["method"] == "getAll") {
    sendReply(transport);
    return;
  }

  if (json_device["method"] == "eraseConfiguration") {
    // Wipe the entire EEPROM area.
    V2Memory::EEPROM::erase();
    V2Memory::Firmware::reboot();
    return;
  }

  if (json_device["method"] == "switchChannel") {
    if (!json_device["channel"].isNull())
      handleSwitchChannel(json_device["channel"]);
    sendReply(transport);
    return;
  }

  if (json_device["method"] == "reboot") {
    if (!json_device["reboot"]["ports"].isNull())
      bootData.n_ports = json_device["reboot"]["ports"];

    V2Memory::Firmware::reboot();
    return;
  }

  if (json_device["method"] == "writeConfiguration") {
    JsonObject config = json_device["configuration"];

    // Write the configuration the the EEPROM.
    if (config) {
      // Common section.
      const char *n = config["name"];
      if (n) {
        if (strlen(n) > 1 && strlen(n) < 32) {
          system.name = n;
          strcpy(_configuration.name, n);

        } else {
          system.name = NULL;
          memset(_configuration.name, 0, sizeof(_configuration.name));
        }
      }

      if (!config["ports"].isNull()) {
        uint8_t p = config["ports"];
        if (p >= 1 && p <= 16) {
          system.ports.configured = p;
          _configuration.ports    = p;
        }
      }

      // Device-specific section.
      if (configuration.size > 0)
        importConfiguration(config);

      writeConfiguration();
    }

    // Reply with the updated configuration.
    sendReply(transport);
    return;
  }

  if (json_device["method"] == "writeFirmware") {
    JsonObject firmware = json_device["firmware"];
    if (firmware) {
      uint32_t offset = firmware["offset"];
      if (offset % V2Memory::Flash::getBlockSize() != 0) {
        sendFirmwareStatus(transport, "invalidOffset");
        return;
      }

      const char *data = firmware["data"];
      union {
        uint32_t block[V2Memory::Flash::getBlockSize() / sizeof(uint32_t)];
        uint8_t bytes[V2Memory::Flash::getBlockSize()];
      };
      uint32_t block_len = V2Cryptography::Base64::decode((const uint8_t *)data, bytes);

      memset(bytes + block_len, 0xff, V2Memory::Flash::getBlockSize() - block_len);
      digitalWrite(LED_BUILTIN, HIGH);
      V2Memory::Firmware::Secondary::writeBlock(offset, block);
      digitalWrite(LED_BUILTIN, LOW);

      // The final message contains our hash over the entire image.
      const char *hash = firmware["hash"];
      if (hash) {
        V2Memory::Firmware::Secondary::copyBootloader();

        if (V2Memory::Firmware::Secondary::verify(offset + block_len, hash)) {
          sendFirmwareStatus(transport, "success");

          // Flush system exclusive message, loop() is no longer called.
          unsigned long usec = micros();
          for (;;) {
            if (!sendSystemExclusive())
              break;

            if ((unsigned long)(micros() - usec) > 100 * 1000)
              break;

            yield();
          }

          // Give the host time to process the message before the USB device disconnects.
          digitalWrite(LED_BUILTIN, HIGH);
          delay(100);

          // System reset with the new firmware image.
          V2Memory::Firmware::Secondary::activate();
        }

        sendFirmwareStatus(transport, "hashMismatch");

      } else
        sendFirmwareStatus(transport, "success");
    }

    return;
  }
}

void V2Device::writeConfiguration() {
  // Common section.
  _configuration.header.size = sizeof(_configuration);
  _configuration.local.magic = configuration.magic;
  _configuration.local.size  = configuration.size;
  V2Memory::EEPROM::write(0, (const uint8_t *)&_configuration, sizeof(_configuration));

  // Device-specific section.
  if (configuration.size > 0)
    V2Memory::EEPROM::write(sizeof(_configuration), (const uint8_t *)configuration.data, configuration.size);
}

bool V2Device::idle() {
  if (!usb.midi.idle())
    return false;

  return true;
}
