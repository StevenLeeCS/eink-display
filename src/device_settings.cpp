#include "device_settings.h"

#include <Preferences.h>
#include <cstring>

#include "network_config.h"

namespace device_settings {
namespace {

constexpr char kPreferencesNamespace[] = "eink-app";
constexpr char kReceiverKey[] = "receiver";
constexpr char kCloudKey[] = "cloud";
constexpr char kProfileKey[] = "profile";
constexpr uint32_t kReceiverMagic = 0x52435652U;  // "RCVR"
constexpr uint8_t kReceiverVersion = 1;
constexpr uint32_t kCloudMagic = 0x434C4F55U;  // "CLOU"
constexpr uint8_t kCloudVersion = 1;
constexpr uint32_t kProfileMagic = 0x50524F46U;  // "PROF"
constexpr uint8_t kProfileVersion = 2;
constexpr uint8_t kMaxNicknameColumns = 12;
constexpr char kDefaultBaiduSpeechUrl[] =
    "https://vop.baidu.com/server_api";
constexpr char kDefaultDeepSeekApiUrl[] =
    "https://api.deepseek.com/chat/completions";
constexpr char kDefaultDeepSeekModel[] = "deepseek-chat";

struct ReceiverRecord {
  uint32_t magic;
  uint8_t version;
  uint16_t port;
  char host[64];
  char path[64];
};

struct CloudRecord {
  uint32_t magic;
  uint8_t version;
  uint8_t deepseekEnabled;
  char baiduApiKey[96];
  char baiduSecretKey[96];
  char baiduSpeechUrl[128];
  char deepseekApiKey[96];
  char deepseekApiUrl[128];
  char deepseekModel[40];
};

struct ProfileRecordV1 {
  uint32_t magic;
  uint8_t version;
  uint8_t functionTestEnabled;
  uint8_t testScene;
  char nickname[32];
};

struct ProfileRecord {
  uint32_t magic;
  uint8_t version;
  uint8_t functionTestEnabled;
  uint8_t aiSceneTestEnabled;
  uint8_t testScene;
  char nickname[32];
};

static_assert(sizeof(ProfileRecordV1) == sizeof(ProfileRecord),
              "Profile v1 migration expects equal record sizes");

ReceiverSettings currentReceiver = {};
CloudSettings currentCloud = {};
ProfileSettings currentProfile = {};
uint32_t currentProfileRevision = 0;
bool initialized = false;

void copyText(char* destination, size_t size, const char* source) {
  if (size == 0) return;
  snprintf(destination, size, "%s", source != nullptr ? source : "");
}

void loadDefaults() {
  copyText(currentReceiver.host, sizeof(currentReceiver.host),
           voice_config::kServerHost);
  currentReceiver.port = voice_config::kServerPort;
  copyText(currentReceiver.path, sizeof(currentReceiver.path),
           voice_config::kServerPath);
}

void loadCloudDefaults() {
  currentCloud = {};
  currentCloud.deepseekEnabled = true;
  copyText(currentCloud.baiduSpeechUrl,
           sizeof(currentCloud.baiduSpeechUrl), kDefaultBaiduSpeechUrl);
  copyText(currentCloud.deepseekApiUrl,
           sizeof(currentCloud.deepseekApiUrl), kDefaultDeepSeekApiUrl);
  copyText(currentCloud.deepseekModel, sizeof(currentCloud.deepseekModel),
           kDefaultDeepSeekModel);
}

void loadProfileDefaults() {
  currentProfile = {};
  copyText(currentProfile.nickname, sizeof(currentProfile.nickname),
           u8"\u670B\u53CB");
  currentProfile.functionTestEnabled = false;
  currentProfile.aiSceneTestEnabled = false;
  currentProfile.testScene = function_area::Scene::Welcome;
}

bool validNickname(const char* nickname) {
  if (nickname == nullptr || nickname[0] == '\0') return false;
  uint8_t columns = 0;
  const uint8_t* cursor = reinterpret_cast<const uint8_t*>(nickname);
  while (*cursor != 0) {
    if (*cursor >= 0x20U && *cursor <= 0x7EU) {
      ++cursor;
      ++columns;
    } else if (*cursor >= 0xC2U && *cursor <= 0xDFU &&
               (cursor[1] & 0xC0U) == 0x80U) {
      cursor += 2;
      columns += 2;
    } else if (*cursor >= 0xE0U && *cursor <= 0xEFU &&
               (cursor[1] & 0xC0U) == 0x80U &&
               (cursor[2] & 0xC0U) == 0x80U &&
               !(*cursor == 0xE0U && cursor[1] < 0xA0U) &&
               !(*cursor == 0xEDU && cursor[1] >= 0xA0U)) {
      cursor += 3;
      columns += 2;
    } else {
      return false;
    }
    if (columns > kMaxNicknameColumns) return false;
  }
  return columns > 0;
}

bool validStoredScene(uint8_t value) {
  return function_area::isValid(static_cast<function_area::Scene>(value)) ||
         value == 3 || value == 5 || value == 6;
}

function_area::Scene normalizeStoredScene(uint8_t value) {
  return value == 3 || value == 5 || value == 6
             ? function_area::Scene::NextAction
             : static_cast<function_area::Scene>(value);
}

bool validHost(const char* host) {
  if (host == nullptr || host[0] == '\0') return false;
  for (const char* character = host; *character != '\0'; ++character) {
    const uint8_t value = static_cast<uint8_t>(*character);
    if (value <= 0x20U || value > 0x7EU || *character == '/' ||
        *character == '\\') {
      return false;
    }
  }
  return true;
}

bool validPath(const char* path) {
  if (path == nullptr || path[0] != '/') return false;
  for (const char* character = path; *character != '\0'; ++character) {
    const uint8_t value = static_cast<uint8_t>(*character);
    if (value <= 0x20U || value > 0x7EU) return false;
  }
  return true;
}

bool validUrl(const char* url) {
  if (url == nullptr ||
      (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0)) {
    return false;
  }
  for (const char* character = url; *character != '\0'; ++character) {
    const uint8_t value = static_cast<uint8_t>(*character);
    if (value <= 0x20U || value > 0x7EU) return false;
  }
  return true;
}

bool validModel(const char* model) {
  if (model == nullptr || model[0] == '\0') return false;
  for (const char* character = model; *character != '\0'; ++character) {
    const uint8_t value = static_cast<uint8_t>(*character);
    if (value <= 0x20U || value > 0x7EU) return false;
  }
  return true;
}

bool validRecord(const ReceiverRecord& record, size_t bytesRead) {
  return bytesRead == sizeof(record) && record.magic == kReceiverMagic &&
         record.version == kReceiverVersion && record.port != 0 &&
         record.host[sizeof(record.host) - 1] == '\0' &&
         record.path[sizeof(record.path) - 1] == '\0' &&
         validHost(record.host) && validPath(record.path);
}

bool validCloudRecord(const CloudRecord& record, size_t bytesRead) {
  return bytesRead == sizeof(record) && record.magic == kCloudMagic &&
         record.version == kCloudVersion && record.deepseekEnabled <= 1 &&
         record.baiduApiKey[sizeof(record.baiduApiKey) - 1] == '\0' &&
         record.baiduSecretKey[sizeof(record.baiduSecretKey) - 1] == '\0' &&
         record.baiduSpeechUrl[sizeof(record.baiduSpeechUrl) - 1] == '\0' &&
         record.deepseekApiKey[sizeof(record.deepseekApiKey) - 1] == '\0' &&
         record.deepseekApiUrl[sizeof(record.deepseekApiUrl) - 1] == '\0' &&
         record.deepseekModel[sizeof(record.deepseekModel) - 1] == '\0' &&
         validUrl(record.baiduSpeechUrl) &&
         validUrl(record.deepseekApiUrl) && validModel(record.deepseekModel);
}

bool validProfileRecord(const ProfileRecord& record, size_t bytesRead) {
  return bytesRead == sizeof(record) && record.magic == kProfileMagic &&
         record.version == kProfileVersion &&
         record.functionTestEnabled <= 1 &&
         record.aiSceneTestEnabled <= 1 &&
         record.nickname[sizeof(record.nickname) - 1] == '\0' &&
         validNickname(record.nickname) &&
         validStoredScene(record.testScene);
}

bool validProfileRecordV1(const ProfileRecordV1& record, size_t bytesRead) {
  return bytesRead == sizeof(record) && record.magic == kProfileMagic &&
         record.version == 1 && record.functionTestEnabled <= 1 &&
         record.nickname[sizeof(record.nickname) - 1] == '\0' &&
         validNickname(record.nickname) &&
         validStoredScene(record.testScene);
}

void applyCloudRecord(const CloudRecord& record) {
  currentCloud.deepseekEnabled = record.deepseekEnabled != 0;
  copyText(currentCloud.baiduApiKey, sizeof(currentCloud.baiduApiKey),
           record.baiduApiKey);
  copyText(currentCloud.baiduSecretKey, sizeof(currentCloud.baiduSecretKey),
           record.baiduSecretKey);
  copyText(currentCloud.baiduSpeechUrl,
           sizeof(currentCloud.baiduSpeechUrl), record.baiduSpeechUrl);
  copyText(currentCloud.deepseekApiKey, sizeof(currentCloud.deepseekApiKey),
           record.deepseekApiKey);
  copyText(currentCloud.deepseekApiUrl,
           sizeof(currentCloud.deepseekApiUrl), record.deepseekApiUrl);
  copyText(currentCloud.deepseekModel, sizeof(currentCloud.deepseekModel),
           record.deepseekModel);
}

void applyProfileRecord(const ProfileRecord& record) {
  copyText(currentProfile.nickname, sizeof(currentProfile.nickname),
           record.nickname);
  currentProfile.functionTestEnabled = record.functionTestEnabled != 0;
  currentProfile.aiSceneTestEnabled = record.aiSceneTestEnabled != 0;
  currentProfile.testScene = normalizeStoredScene(record.testScene);
}

void applyProfileRecordV1(const ProfileRecordV1& record) {
  copyText(currentProfile.nickname, sizeof(currentProfile.nickname),
           record.nickname);
  currentProfile.functionTestEnabled = record.functionTestEnabled != 0;
  currentProfile.aiSceneTestEnabled = false;
  currentProfile.testScene = normalizeStoredScene(record.testScene);
}

}  // namespace

void begin() {
  if (initialized) return;
  initialized = true;
  loadDefaults();
  loadCloudDefaults();
  loadProfileDefaults();

  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, true)) {
    Serial.println("WARNING: Could not open device settings; using defaults.");
    return;
  }

  ReceiverRecord receiverRecord = {};
  const size_t receiverSize = preferences.getBytesLength(kReceiverKey);
  const size_t receiverBytes =
      receiverSize == sizeof(receiverRecord)
          ? preferences.getBytes(kReceiverKey, &receiverRecord,
                                 sizeof(receiverRecord))
          : 0;
  CloudRecord cloudRecord = {};
  const size_t cloudSize = preferences.getBytesLength(kCloudKey);
  const size_t cloudBytes =
      cloudSize == sizeof(cloudRecord)
          ? preferences.getBytes(kCloudKey, &cloudRecord, sizeof(cloudRecord))
          : 0;
  uint8_t profileBytesBuffer[sizeof(ProfileRecord)] = {};
  const size_t profileSize = preferences.getBytesLength(kProfileKey);
  const size_t profileBytes =
      profileSize == sizeof(profileBytesBuffer)
          ? preferences.getBytes(kProfileKey, profileBytesBuffer,
                                 sizeof(profileBytesBuffer))
          : 0;
  preferences.end();
  if (validRecord(receiverRecord, receiverBytes)) {
    copyText(currentReceiver.host, sizeof(currentReceiver.host),
             receiverRecord.host);
    currentReceiver.port = receiverRecord.port;
    copyText(currentReceiver.path, sizeof(currentReceiver.path),
             receiverRecord.path);
  }
  if (validCloudRecord(cloudRecord, cloudBytes)) {
    applyCloudRecord(cloudRecord);
  }
  ProfileRecord profileRecord = {};
  memcpy(&profileRecord, profileBytesBuffer, sizeof(profileRecord));
  ProfileRecordV1 profileRecordV1 = {};
  memcpy(&profileRecordV1, profileBytesBuffer, sizeof(profileRecordV1));
  if (validProfileRecord(profileRecord, profileBytes)) {
    applyProfileRecord(profileRecord);
  } else if (validProfileRecordV1(profileRecordV1, profileBytes)) {
    applyProfileRecordV1(profileRecordV1);
  }
  currentProfileRevision = 1;
}

const ReceiverSettings& receiver() {
  begin();
  return currentReceiver;
}

bool saveReceiver(const String& host, uint16_t port, const String& path) {
  begin();
  if (host.isEmpty() || host.length() >= sizeof(currentReceiver.host) ||
      !validHost(host.c_str()) || port == 0 || path.isEmpty() ||
      path.length() >= sizeof(currentReceiver.path) ||
      !validPath(path.c_str())) {
    return false;
  }

  ReceiverRecord record = {};
  record.magic = kReceiverMagic;
  record.version = kReceiverVersion;
  record.port = port;
  copyText(record.host, sizeof(record.host), host.c_str());
  copyText(record.path, sizeof(record.path), path.c_str());

  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) return false;
  const size_t bytesWritten =
      preferences.putBytes(kReceiverKey, &record, sizeof(record));
  preferences.end();
  if (bytesWritten != sizeof(record)) return false;

  copyText(currentReceiver.host, sizeof(currentReceiver.host), record.host);
  currentReceiver.port = record.port;
  copyText(currentReceiver.path, sizeof(currentReceiver.path), record.path);
  return true;
}

void resetReceiver() {
  begin();
  Preferences preferences;
  if (preferences.begin(kPreferencesNamespace, false)) {
    preferences.remove(kReceiverKey);
    preferences.end();
  }
  loadDefaults();
}

const CloudSettings& cloud() {
  begin();
  return currentCloud;
}

bool saveCloud(const CloudSettingsUpdate& update,
               bool preserveEmptySecrets) {
  begin();
  if (update.baiduApiKey.length() >= sizeof(currentCloud.baiduApiKey) ||
      update.baiduSecretKey.length() >=
          sizeof(currentCloud.baiduSecretKey) ||
      update.baiduSpeechUrl.isEmpty() ||
      update.baiduSpeechUrl.length() >=
          sizeof(currentCloud.baiduSpeechUrl) ||
      !validUrl(update.baiduSpeechUrl.c_str()) ||
      update.deepseekApiKey.length() >=
          sizeof(currentCloud.deepseekApiKey) ||
      update.deepseekApiUrl.isEmpty() ||
      update.deepseekApiUrl.length() >=
          sizeof(currentCloud.deepseekApiUrl) ||
      !validUrl(update.deepseekApiUrl.c_str()) ||
      update.deepseekModel.isEmpty() ||
      update.deepseekModel.length() >= sizeof(currentCloud.deepseekModel) ||
      !validModel(update.deepseekModel.c_str())) {
    return false;
  }

  CloudRecord record = {};
  record.magic = kCloudMagic;
  record.version = kCloudVersion;
  record.deepseekEnabled = update.deepseekEnabled ? 1 : 0;
  const char* baiduKey =
      preserveEmptySecrets && update.baiduApiKey.isEmpty()
          ? currentCloud.baiduApiKey
          : update.baiduApiKey.c_str();
  const char* baiduSecret =
      preserveEmptySecrets && update.baiduSecretKey.isEmpty()
          ? currentCloud.baiduSecretKey
          : update.baiduSecretKey.c_str();
  const char* deepseekKey =
      preserveEmptySecrets && update.deepseekApiKey.isEmpty()
          ? currentCloud.deepseekApiKey
          : update.deepseekApiKey.c_str();
  copyText(record.baiduApiKey, sizeof(record.baiduApiKey), baiduKey);
  copyText(record.baiduSecretKey, sizeof(record.baiduSecretKey), baiduSecret);
  copyText(record.baiduSpeechUrl, sizeof(record.baiduSpeechUrl),
           update.baiduSpeechUrl.c_str());
  copyText(record.deepseekApiKey, sizeof(record.deepseekApiKey), deepseekKey);
  copyText(record.deepseekApiUrl, sizeof(record.deepseekApiUrl),
           update.deepseekApiUrl.c_str());
  copyText(record.deepseekModel, sizeof(record.deepseekModel),
           update.deepseekModel.c_str());

  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) return false;
  const size_t bytesWritten =
      preferences.putBytes(kCloudKey, &record, sizeof(record));
  preferences.end();
  if (bytesWritten != sizeof(record)) return false;
  applyCloudRecord(record);
  return true;
}

void resetCloud() {
  begin();
  Preferences preferences;
  if (preferences.begin(kPreferencesNamespace, false)) {
    preferences.remove(kCloudKey);
    preferences.end();
  }
  loadCloudDefaults();
}

const ProfileSettings& profile() {
  begin();
  return currentProfile;
}

bool saveProfile(const String& nickname, bool functionTestEnabled,
                 bool aiSceneTestEnabled, function_area::Scene testScene) {
  begin();
  String normalizedNickname = nickname;
  normalizedNickname.trim();
  if (normalizedNickname.isEmpty() ||
      normalizedNickname.length() >= sizeof(currentProfile.nickname) ||
      !validNickname(normalizedNickname.c_str()) ||
      !function_area::isValid(testScene)) {
    return false;
  }

  ProfileRecord record = {};
  record.magic = kProfileMagic;
  record.version = kProfileVersion;
  record.functionTestEnabled = functionTestEnabled ? 1 : 0;
  record.aiSceneTestEnabled = aiSceneTestEnabled ? 1 : 0;
  record.testScene = static_cast<uint8_t>(testScene);
  copyText(record.nickname, sizeof(record.nickname),
           normalizedNickname.c_str());

  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) return false;
  const size_t bytesWritten =
      preferences.putBytes(kProfileKey, &record, sizeof(record));
  preferences.end();
  if (bytesWritten != sizeof(record)) return false;

  applyProfileRecord(record);
  ++currentProfileRevision;
  return true;
}

uint32_t profileRevision() {
  begin();
  return currentProfileRevision;
}

}  // namespace device_settings
