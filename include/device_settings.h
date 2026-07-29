#pragma once

#include <Arduino.h>
#include <cstdint>

namespace device_settings {

struct ReceiverSettings {
  char host[64];
  uint16_t port;
  char path[64];
};

struct CloudSettings {
  bool deepseekEnabled;
  char baiduApiKey[96];
  char baiduSecretKey[96];
  char baiduSpeechUrl[128];
  char deepseekApiKey[96];
  char deepseekApiUrl[128];
  char deepseekModel[40];
};

struct CloudSettingsUpdate {
  bool deepseekEnabled;
  String baiduApiKey;
  String baiduSecretKey;
  String baiduSpeechUrl;
  String deepseekApiKey;
  String deepseekApiUrl;
  String deepseekModel;
};

void begin();
const ReceiverSettings& receiver();
bool saveReceiver(const String& host, uint16_t port, const String& path);
void resetReceiver();
const CloudSettings& cloud();
bool saveCloud(const CloudSettingsUpdate& update,
               bool preserveEmptySecrets = true);
void resetCloud();

}  // namespace device_settings
