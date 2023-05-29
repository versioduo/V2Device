// © Kay Sievers <kay@versioduo.com>, 2020-2022
// SPDX-License-Identifier: Apache-2.0

#include "V2Device.h"
#include <V2Base.h>

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
    usb.ports.enableAccess = false;
  }

  // The number of MIDI ports to export to the host
  struct {
    struct {
      bool enableAccess;
    } ports;
  } usb;

private:
  uint32_t _magic;
} bootData __attribute__((section(".noinit")));

bool V2Device::readEEPROM(bool dryrun) {
  struct EEPROM *eeprom = (struct EEPROM *)V2Base::Memory::EEPROM::getStart();
  // Check our magic, all bytes are 0xff after chip erase.
  if (eeprom->header.magic != _eeprom.header.magic)
    return false;

  if (eeprom->header.size <= sizeof(struct EEPROM::Header))
    return false;

  if (!dryrun)
    memcpy(&_eeprom, eeprom, min(eeprom->header.size, sizeof(_eeprom)));

  if (_eeprom.usb.name[0] != '\0')
    usb.name = _eeprom.usb.name;

  // Device-specific section.
  if (eeprom->local.magic != usb.pid || !configuration.data || eeprom->local.size == 0)
    return true;

  const void *data = (const uint8_t *)V2Base::Memory::EEPROM::getStart() + eeprom->header.size;

  // Try to import an older version of the configuration.
  if (eeprom->local.version != configuration.version) {
    if (!dryrun)
      handleEEPROM(eeprom->local.version, data, eeprom->local.size);

    return true;
  }

  if (!dryrun)
    memcpy(configuration.data, data, min(eeprom->local.size, configuration.size));

  return true;
}

void V2Device::begin() {
  V2MIDI::Port::begin();
  usb.midi.begin();

  // The priority needs to be lower than the SERCOM priorities.
  _ledTimer.begin(std::bind(&V2LED::Basic::tick, &led));
  _ledTimer.setPriority(3);

  if (V2Base::Memory::Flash::UserPage::update()) {
    // Reboot to enable the new settings.
    delay(100);
    V2Base::Memory::Firmware::reboot();
  }

  _boot.id = V2Base::Cryptography::Random::read();

  // Do not block in GetAll(), it takes ~80ms.
  V2Base::Memory::Firmware::calculateHash(V2Base::Memory::Firmware::getStart(),
                                          V2Base::Memory::Firmware::getSize(),
                                          _firmware.hash);

  // Read a possible config from the previous boot cycle.
  if (bootData.usb.ports.enableAccess)
    usb.ports.enableAccess = true;

  bootData.clear();

#ifdef PIN_REVISION_BITS
  // The revision number is composed of pins which are either floating or
  // connected to ground. A ground connection represents a logical high.
  {
    for (uint8_t i = 0; i < PIN_REVISION_BITS; i++)
      pinMode(PIN_REVISION + i, INPUT_PULLUP);

    for (uint8_t i = 0; i < PIN_REVISION_BITS; i++) {
      if (digitalRead(PIN_REVISION + i))
        continue;

      system.revision |= 1 << i;
    }

    for (uint8_t i = 0; i < PIN_REVISION_BITS; i++)
      pinMode(PIN_REVISION + i, INPUT);
  }
#endif

  readEEPROM();
  handleInit();

  // Set USB device name, the default is provided by the board package, the metadata
  // provides a product name, a custom name might be stored in the EEPROM.
  const char *name = usb.name ? usb.name : metadata.product;
  usb.midi.setName(name);
  if (system.configure && memcmp(system.configure, "https://", 8) == 0)
    usb.midi.setConfigureURL(system.configure, name);

  // Set USB MIDI ports.
  if (usb.ports.enableAccess)
    usb.ports.current = (usb.ports.access > 0) ? usb.ports.access : 16;

  else if (_eeprom.usb.ports > 0)
    usb.ports.current = _eeprom.usb.ports;

  else if (usb.ports.standard > 0)
    usb.ports.current = usb.ports.standard;

  if (usb.ports.current > 1)
    usb.midi.setPorts(usb.ports.current);

  // Operating systems/services/apps get confused if the number
  // of ports changes between device connections; some hang, some
  // don't probe the device again and ignore the new number of ports.
  //
  // To work around it, let the USB ID depend on the number of ports.
  const uint16_t vid = _eeprom.usb.vid > 0 ? _eeprom.usb.vid : usb.vid;
  const uint16_t pid = _eeprom.usb.pid > 0 ? _eeprom.usb.pid : usb.pid;
  usb.midi.setVIDPID(vid, pid + usb.ports.current - 1);

  usb.midi.setVersion(V2DeviceMetadata.version);
  usb.midi.attach();

  // Sleep mode IDLE, wait for interrupts.
  V2Base::Power::setSleepMode(V2Base::Power::Mode::Idle);
}

void V2Device::reset() {
  led.reset();
  resetSystemExclusive();
  handleReset();
}

void V2Device::loop() {
  led.loop();
  loopSystemExclusive();
  handleLoop();
}

// Reply with message to indicate that we are ready for the next packet.
void V2Device::sendFirmwareStatus(V2MIDI::Transport *transport, const char *status) {
  uint8_t *reply = getSystemExclusiveBuffer();
  uint32_t len   = 0;

  // 0x7d == SysEx research/private ID
  reply[len++] = (uint8_t)V2MIDI::Packet::Status::SystemExclusive;
  reply[len++] = 0x7d;

  StaticJsonDocument<1024> json;
  JsonObject jsonDevice   = json.createNestedObject("com.versioduo.device");
  jsonDevice["token"]     = _boot.id;
  JsonObject jsonFirmware = jsonDevice.createNestedObject("firmware");
  jsonFirmware["status"]  = status;
  len += serializeJson(json, (char *)reply + len, 1024);

  reply[len++] = (uint8_t)V2MIDI::Packet::Status::SystemExclusiveEnd;
  sendSystemExclusive(transport, len);
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

// Escape unicode to fit into a 7 bit byte stream.
static uint32_t escapeJSON(const uint8_t *jsonBuffer, uint32_t jsonLen, uint8_t *buffer, uint32_t size) {
  uint32_t bufferLen = 0;

  for (uint32_t i = 0; i < jsonLen; i++) {
    if (jsonBuffer[i] > 0x7f) {
      uint32_t codepoint;
      uint8_t len = utf8Codepoint(jsonBuffer + i, &codepoint);
      if (len < 0)
        continue;

      // Advance the additional UTF8 characters for this codepoint.
      i += len - 1;

      if (codepoint < 0xffff) {
        if (bufferLen + 7 > size)
          return 0;

        bufferLen += sprintf((char *)buffer + bufferLen, "\\u%04x", codepoint);

      } else {
        if (bufferLen + 13 > size)
          return 0;

        codepoint -= 0x10000;
        uint16_t surrogate1 = (codepoint >> 10) + 0xd800;
        uint16_t surrogate2 = (codepoint & 0x3ff) + 0xdc00;
        bufferLen += sprintf((char *)buffer + bufferLen, "\\u%04x\\u%04x", surrogate1, surrogate2);
      }

    } else {
      if (bufferLen >= size)
        return 0;

      buffer[bufferLen++] = jsonBuffer[i];
    }
  }

  return bufferLen;
}

void addStatistics(JsonObject json, V2MIDI::Port::Counter *counter) {
  json["packet"] = counter->packet;

  if (counter->note > 0)
    json["note"] = counter->note;

  if (counter->noteOff > 0)
    json["noteOff"] = counter->noteOff;

  if (counter->aftertouch > 0)
    json["aftertouch"] = counter->aftertouch;

  if (counter->control > 0)
    json["control"] = counter->control;

  if (counter->program > 0)
    json["program"] = counter->program;

  if (counter->aftertouchChannel > 0)
    json["aftertouchChannel"] = counter->aftertouchChannel;

  if (counter->pitchbend > 0)
    json["pitchbend"] = counter->pitchbend;

  if (counter->system.exclusive > 0 || counter->system.reset > 0 || counter->system.clock.tick > 0) {
    JsonObject system = json.createNestedObject("system");
    if (counter->system.exclusive > 0)
      system["exclusive"] = counter->system.exclusive;

    if (counter->system.reset > 0)
      system["reset"] = counter->system.reset;

    if (counter->system.clock.tick > 0) {
      JsonObject clock = system.createNestedObject("clock");
      clock["tick"]    = counter->system.clock.tick;
    }
  }
}

// Send the current data as a SystemExclusive, JSON message.
void V2Device::sendReply(V2MIDI::Transport *transport) {
  uint8_t *reply = getSystemExclusiveBuffer();
  uint32_t len   = 0;

  // 0x7d == SysEx research/private ID
  reply[len++] = (uint8_t)V2MIDI::Packet::Status::SystemExclusive;
  reply[len++] = 0x7d;

  DynamicJsonDocument json(_sysexSize + 4096);
  JsonObject jsonDevice = json.createNestedObject("com.versioduo.device");

  // Requests and replies contain the device's current bootID.
  jsonDevice["token"] = _boot.id;

  {
    JsonObject jsonMeta = jsonDevice.createNestedObject("metadata");
    if (metadata.product)
      jsonMeta["product"] = metadata.product;

    if (metadata.description)
      jsonMeta["description"] = metadata.description;

    if (metadata.vendor)
      jsonMeta["vendor"] = metadata.vendor;

    if (metadata.home)
      jsonMeta["home"] = metadata.home;

    {
      char serial[33];
      usb.midi.readSerial(serial);
      jsonMeta["serial"] = serial;
    }

    jsonMeta["version"] = V2DeviceMetadata.version;
    exportMetadata(jsonMeta);
  }

  {
    JsonArray jsonLinks = jsonDevice.createNestedArray("links");
    exportLinks(jsonLinks);
  }

  {
    JsonObject jsonHelp = jsonDevice.createNestedObject("help");
    if (help.device)
      jsonHelp["device"] = help.device;

    if (help.configuration)
      jsonHelp["configuration"] = help.configuration;
  }

  {
    JsonObject jsonSystem = jsonDevice.createNestedObject("system");
    if (usb.name)
      jsonSystem["name"] = usb.name;

    {
      JsonObject jsonBoot = jsonSystem.createNestedObject("boot");
      jsonBoot["uptime"]  = (uint32_t)(millis() / 1000);
      jsonBoot["id"]      = _boot.id;
    }

    {
      JsonObject jsonFirmware = jsonSystem.createNestedObject("firmware");
      if (system.download)
        jsonFirmware["download"] = system.download;

      if (system.configure)
        jsonFirmware["configure"] = system.configure;

      jsonFirmware["id"]    = V2DeviceMetadata.id;
      jsonFirmware["board"] = V2DeviceMetadata.board;
      jsonFirmware["hash"]  = _firmware.hash;
      jsonFirmware["start"] = V2Base::Memory::Firmware::getStart();
      jsonFirmware["size"]  = V2Base::Memory::Firmware::getSize();
    }

    {
      JsonObject jsonHardware = jsonSystem.createNestedObject("hardware");

      {
        // The end of the bootloader contains an array of four offsets/pointers.
        const uint32_t *info = (uint32_t *)V2Base::Memory::Firmware::getStart() - 4;

        // The first entry is the location of our metadata.
        const char *metadata = (const char *)info[0];
        StaticJsonDocument<2 * 1024> jsonMetadata;
        if (deserializeJson(jsonMetadata, metadata))
          return;

        JsonObject jsonBootloader = jsonMetadata["com.versioduo.bootloader"];
        if (!jsonBootloader)
          return;

        if (!jsonBootloader["board"])
          return;

        jsonHardware["board"] = jsonBootloader["board"];
      }

      if (system.revision > 0)
        jsonHardware["revision"] = system.revision;

      {
        JsonObject jsonRam = jsonHardware.createNestedObject("ram");
        jsonRam["size"]    = V2Base::Memory::RAM::getSize();
        jsonRam["free"]    = V2Base::Memory::RAM::getFree();
      }

      {
        JsonObject jsonFlash = jsonHardware.createNestedObject("flash");
        jsonFlash["size"]    = V2Base::Memory::Flash::getSize();
      }

      {
        JsonObject jsonEeprom = jsonHardware.createNestedObject("eeprom");
        jsonEeprom["size"]    = V2Base::Memory::EEPROM::getSize();
        jsonEeprom["used"]    = readEEPROM(true);
      }

      {
        JsonObject jsonUsb = jsonHardware.createNestedObject("usb");
        {
          JsonObject jsonHost  = jsonUsb.createNestedObject("connection");
          jsonHost["active"]   = usb.midi.connected();
          jsonHost["sequence"] = usb.midi.getConnectionSequence();
        }

        jsonUsb["vid"] = _eeprom.usb.vid > 0 ? _eeprom.usb.vid : usb.vid;
        jsonUsb["pid"] = _eeprom.usb.pid > 0 ? _eeprom.usb.pid : usb.pid;

        if (usb.ports.standard > 0) {
          JsonObject jsonPorts  = jsonUsb.createNestedObject("ports");
          jsonPorts["standard"] = usb.ports.standard;
          if (usb.ports.access > 0)
            jsonPorts["access"] = usb.ports.access;
          jsonPorts["current"] = usb.ports.current;
        }
      }
    }

    JsonObject jsonMidi = jsonSystem.createNestedObject("midi");
    {
      JsonObject jsonIn = jsonMidi.createNestedObject("input");
      addStatistics(jsonIn, &_statistics.input);

      JsonObject jsonOut = jsonMidi.createNestedObject("output");
      addStatistics(jsonOut, &_statistics.output);
    }

    if (link) {
      JsonObject jsonLink = jsonSystem.createNestedObject("link");
      if (link->plug) {
        JsonObject jsonPlug = jsonLink.createNestedObject("plug");
        jsonPlug["input"]   = link->plug->statistics.input;
        jsonPlug["output"]  = link->plug->statistics.output;
      }

      if (link->socket) {
        JsonObject jsonSocket = jsonLink.createNestedObject("socket");
        jsonSocket["input"]   = link->socket->statistics.input;
        jsonSocket["output"]  = link->socket->statistics.output;
      }
    }

    if (serial) {
      JsonObject jsonSerial = jsonSystem.createNestedObject("serial");
      jsonSerial["input"]   = serial->statistics.input;
      jsonSerial["output"]  = serial->statistics.output;
    }

    exportSystem(jsonSystem);
  }

  JsonArray settings = jsonDevice.createNestedArray("settings");
  exportSettings(settings);

  {
    JsonObject config  = jsonDevice.createNestedObject("configuration");
    config["#usb"]     = "USB Settings";
    JsonObject jsonUsb = config.createNestedObject("usb");
    jsonUsb["#name"]   = "Device Name";
    jsonUsb["name"]    = _eeprom.usb.name;

    jsonUsb["#vid"] = "USB Vendor ID";
    jsonUsb["vid"]  = _eeprom.usb.vid;

    jsonUsb["#pid"] = "USB Product ID";
    jsonUsb["pid"]  = _eeprom.usb.pid;

    if (usb.ports.standard > 0) {
      jsonUsb["#ports"] = "Number of MIDI ports";
      jsonUsb["ports"]  = _eeprom.usb.ports;
    }

    exportConfiguration(config);
  }

  JsonObject input = jsonDevice.createNestedObject("input");
  exportInput(input);
  if (input.begin() == input.end())
    jsonDevice.remove("input");

  JsonObject output = jsonDevice.createNestedObject("output");
  exportOutput(output);
  if (output.begin() == output.end())
    jsonDevice.remove("output");

  {
    uint8_t jsonBuffer[_sysexSize];
    uint32_t jsonLen = serializeJson(json, (char *)jsonBuffer, _sysexSize);
    len += escapeJSON(jsonBuffer, jsonLen, reply + len, _sysexSize - len);
  }

  reply[len++] = (uint8_t)V2MIDI::Packet::Status::SystemExclusiveEnd;
  sendSystemExclusive(transport, len);
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
  DynamicJsonDocument json(_sysexSize + 4096);
  if (deserializeJson(json, buffer + 2, len - 1))
    return;

  // Only handle requests for our interface.
  JsonObject jsonDevice = json["com.versioduo.device"];
  if (!jsonDevice)
    return;

  // Requests and replies contain the device's current bootID. The token prevents
  // devices from accepting messages intended for a different device, or messages
  // addressed to the same device but a different boot cycle.
  if (!jsonDevice["token"].isNull() && jsonDevice["token"] != _boot.id)
    return;

  if (jsonDevice["method"] == "getAll") {
    sendReply(transport);
    return;
  }

  if (jsonDevice["method"] == "eraseConfiguration") {
    // Wipe the entire EEPROM area.
    V2Base::Memory::EEPROM::erase();
    V2Base::Memory::Firmware::reboot();
    return;
  }

  if (jsonDevice["method"] == "switchChannel") {
    if (!jsonDevice["channel"].isNull())
      handleSwitchChannel(jsonDevice["channel"]);
    sendReply(transport);
    return;
  }

  if (jsonDevice["method"] == "reboot") {
    V2Base::Memory::Firmware::reboot();
    return;
  }

  if (jsonDevice["method"] == "rebootWithPorts") {
    bootData.usb.ports.enableAccess = true;
    V2Base::Memory::Firmware::reboot();
    return;
  }

  // Write the configuration the the EEPROM.
  if (jsonDevice["method"] == "writeConfiguration") {
    // The data in enclosed in an object to prevent name clashes with the
    // calling convention.
    JsonObject config = jsonDevice["configuration"];
    if (config) {
      JsonObject jsonUsb = config["usb"];
      if (jsonUsb) {
        const char *n = jsonUsb["name"];
        if (n) {
          if (strlen(n) > 1 && strlen(n) < 32) {
            usb.name = n;
            strcpy(_eeprom.usb.name, n);

          } else {
            usb.name = NULL;
            memset(_eeprom.usb.name, 0, sizeof(_eeprom.usb.name));
          }
        }

        if (!jsonUsb["vid"].isNull()) {
          uint16_t vid    = jsonUsb["vid"];
          _eeprom.usb.vid = vid;
        }

        if (!jsonUsb["pid"].isNull()) {
          uint16_t pid    = jsonUsb["pid"];
          _eeprom.usb.pid = pid;
        }

        if (!jsonUsb["ports"].isNull()) {
          uint8_t p = jsonUsb["ports"];
          if (p <= 16)
            _eeprom.usb.ports = p;
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

  if (jsonDevice["method"] == "writeFirmware") {
    // The data in enclosed in an object to prevent name clashes with the
    // calling convention.
    JsonObject firmware = jsonDevice["firmware"];
    if (firmware) {
      uint32_t offset = firmware["offset"];
      if (offset % V2Base::Memory::Flash::getBlockSize() != 0) {
        sendFirmwareStatus(transport, "invalidOffset");
        return;
      }

      const char *data = firmware["data"];
      union {
        uint32_t block[V2Base::Memory::Flash::getBlockSize() / sizeof(uint32_t)];
        uint8_t bytes[V2Base::Memory::Flash::getBlockSize()];
      };
      uint32_t blockLen = V2Base::Text::Base64::decode((const uint8_t *)data, bytes);

      memset(bytes + blockLen, 0xff, V2Base::Memory::Flash::getBlockSize() - blockLen);
      led.setBrightness(0.3);
      V2Base::Memory::Firmware::Secondary::writeBlock(offset, block);
      led.setBrightness(0.1);

      // The final message contains our hash over the entire image.
      const char *hash = firmware["hash"];
      if (hash) {
        V2Base::Memory::Firmware::Secondary::copyBootloader();

        if (V2Base::Memory::Firmware::Secondary::verify(offset + blockLen, hash)) {
          sendFirmwareStatus(transport, "success");

          // Flush system exclusive message, loop() is no longer called.
          uint32_t usec = V2Base::getUsec();
          for (;;) {
            if (loopSystemExclusive() == 0)
              break;

            if ((uint32_t)(V2Base::getUsec() - usec) > 100 * 1000)
              break;

            yield();
          }

          // Give the host time to process the message before the USB device disconnects.
          led.setBrightness(1);
          delay(100);

          // System reset with the new firmware image.
          V2Base::Memory::Firmware::Secondary::activate();
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
  _eeprom.local.magic   = usb.pid;
  _eeprom.local.version = configuration.version;
  _eeprom.local.size    = configuration.size;
  V2Base::Memory::EEPROM::write(0, (const uint8_t *)&_eeprom, sizeof(_eeprom));

  // Device-specific section.
  if (configuration.size > 0)
    V2Base::Memory::EEPROM::write(sizeof(_eeprom), (const uint8_t *)configuration.data, configuration.size);
}

bool V2Device::idle() {
  if (!usb.midi.idle())
    return false;

  return true;
}
