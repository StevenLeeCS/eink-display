#pragma once

#include <cstdint>

namespace voice_config {

constexpr char kWifiSsid[] = "YOUR_WIFI_SSID";
constexpr char kWifiPassword[] = "YOUR_WIFI_PASSWORD";

// Use the LAN IPv4 address of the computer running audio_receiver.py.
// Do not use localhost or 127.0.0.1 here.
constexpr char kServerHost[] = "192.168.1.100";
constexpr uint16_t kServerPort = 18000;
constexpr char kServerPath[] = "/audio";

}  // namespace voice_config
