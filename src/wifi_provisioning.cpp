#include "wifi_provisioning.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <cstring>

#include "device_settings.h"
#include "task_store.h"

extern const uint8_t admin_html_start[]
    asm("_binary_assets_admin_html_start");
extern const uint8_t admin_html_end[] asm("_binary_assets_admin_html_end");

namespace wifi_provisioning {
namespace {

constexpr char kPreferencesNamespace[] = "eink-wifi";
constexpr char kCredentialsKey[] = "credentials";
constexpr uint32_t kCredentialsMagic = 0x57494649U;  // "WIFI"
constexpr uint8_t kCredentialsVersion = 1;
constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr uint16_t kDnsPort = 53;
constexpr uint16_t kHttpPort = 80;

String savedSsid;
String savedPassword;
WebServer adminServer(kHttpPort);
DNSServer adminDnsServer;
bool adminServerStarted = false;
bool captivePortalMode = false;

struct CredentialsRecord {
  uint32_t magic;
  uint8_t version;
  uint8_t ssidLength;
  uint8_t passwordLength;
  char ssid[33];
  char password[64];
};

String jsonEscape(const String& value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char character = value[i];
    switch (character) {
      case '\\':
        escaped += F("\\\\");
        break;
      case '"':
        escaped += F("\\\"");
        break;
      case '\b':
        escaped += F("\\b");
        break;
      case '\f':
        escaped += F("\\f");
        break;
      case '\n':
        escaped += F("\\n");
        break;
      case '\r':
        escaped += F("\\r");
        break;
      case '\t':
        escaped += F("\\t");
        break;
      default:
        if (static_cast<uint8_t>(character) < 0x20U) {
          char encoded[7];
          snprintf(encoded, sizeof(encoded), "\\u%04X",
                   static_cast<unsigned>(static_cast<uint8_t>(character)));
          escaped += encoded;
        } else {
          escaped += character;
        }
        break;
    }
  }
  return escaped;
}

void appendUint64(String& output, uint64_t value) {
  char encoded[24];
  snprintf(encoded, sizeof(encoded), "%llu",
           static_cast<unsigned long long>(value));
  output += encoded;
}

void sendJson(WebServer& server, int status, const String& body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(status, "application/json; charset=utf-8", body);
}

void sendMessage(WebServer& server, int status, const __FlashStringHelper* message) {
  String response = F("{\"ok\":");
  response += status >= 200 && status < 300 ? F("true") : F("false");
  response += F(",\"message\":\"");
  response += message;
  response += F("\"}");
  sendJson(server, status, response);
}

void loadCredentials() {
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, true)) {
    Serial.println("ERROR: Could not open Wi-Fi preferences.");
    savedSsid = "";
    savedPassword = "";
    return;
  }

  CredentialsRecord record = {};
  const bool validSize =
      preferences.getBytesLength(kCredentialsKey) == sizeof(record);
  const size_t bytesRead =
      validSize
          ? preferences.getBytes(kCredentialsKey, &record, sizeof(record))
          : 0;
  preferences.end();

  const bool valid =
      bytesRead == sizeof(record) && record.magic == kCredentialsMagic &&
      record.version == kCredentialsVersion && record.ssidLength > 0 &&
      record.ssidLength <= 32 && record.passwordLength <= 63 &&
      record.ssid[record.ssidLength] == '\0' &&
      record.password[record.passwordLength] == '\0';
  if (!valid) {
    savedSsid = "";
    savedPassword = "";
    return;
  }
  savedSsid = record.ssid;
  savedPassword = record.password;
}

bool saveCredentials(const String& ssid, const String& password) {
  CredentialsRecord record = {};
  record.magic = kCredentialsMagic;
  record.version = kCredentialsVersion;
  record.ssidLength = static_cast<uint8_t>(ssid.length());
  record.passwordLength = static_cast<uint8_t>(password.length());
  memcpy(record.ssid, ssid.c_str(), record.ssidLength);
  memcpy(record.password, password.c_str(), record.passwordLength);

  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) return false;
  const size_t bytesWritten =
      preferences.putBytes(kCredentialsKey, &record, sizeof(record));
  preferences.end();
  if (bytesWritten != sizeof(record)) return false;
  savedSsid = ssid;
  savedPassword = password;
  return true;
}

bool clearCredentials() {
  Preferences preferences;
  if (!preferences.begin(kPreferencesNamespace, false)) return false;
  const bool removed = preferences.remove(kCredentialsKey);
  preferences.end();
  savedSsid = "";
  savedPassword = "";
  return removed;
}

bool connectStation(const String& ssid, const String& password) {
  if (ssid.isEmpty()) return false;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(ssid.c_str(), password.c_str());
  Serial.printf("Connecting to Wi-Fi: %s", ssid.c_str());

  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - started < kConnectTimeoutMs) {
    Serial.print('.');
    delay(500);
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("ERROR: Wi-Fi connection failed (status %d).\n",
                  static_cast<int>(WiFi.status()));
    WiFi.disconnect(false, false);
    return false;
  }

  Serial.print("Wi-Fi connected, device IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

void sendAdminPage(WebServer& server) {
  const size_t length =
      static_cast<size_t>(admin_html_end - admin_html_start);
  server.send_P(200, PSTR("text/html; charset=utf-8"),
                reinterpret_cast<PGM_P>(admin_html_start), length);
}

void sendNotFound(WebServer& server) {
  if (captivePortalMode) {
    sendAdminPage(server);
    return;
  }
  sendJson(server, 404, F("{\"ok\":false,\"message\":\"Not found\"}"));
}

void registerStatusRoutes(WebServer& server) {
  server.on("/api/status", HTTP_GET, [&server]() {
    const bool connected = WiFi.status() == WL_CONNECTED;
    const device_settings::ReceiverSettings& receiver =
        device_settings::receiver();
    const device_settings::CloudSettings& cloud = device_settings::cloud();
    const device_settings::ProfileSettings& profile =
        device_settings::profile();
    String response;
    response.reserve(1024);
    response += F("{\"connected\":");
    response += connected ? F("true") : F("false");
    response += F(",\"ssid\":\"");
    response += jsonEscape(connected ? WiFi.SSID() : savedSsid);
    response += F("\",\"station_ip\":\"");
    if (connected) response += WiFi.localIP().toString();
    response += F("\",\"ap_ip\":\"");
    response += WiFi.softAPIP().toString();
    response += F("\",\"rssi\":");
    response += connected ? String(WiFi.RSSI()) : String(0);
    response += F(",\"free_heap\":");
    response += ESP.getFreeHeap();
    response += F(",\"receiver\":{\"host\":\"");
    response += jsonEscape(receiver.host);
    response += F("\",\"port\":");
    response += receiver.port;
    response += F(",\"path\":\"");
    response += jsonEscape(receiver.path);
    response += F("\"},\"cloud\":{\"baidu_api_key_set\":");
    response += cloud.baiduApiKey[0] != '\0' ? F("true") : F("false");
    response += F(",\"baidu_secret_key_set\":");
    response += cloud.baiduSecretKey[0] != '\0' ? F("true") : F("false");
    response += F(",\"baidu_speech_url\":\"");
    response += jsonEscape(cloud.baiduSpeechUrl);
    response += F("\",\"deepseek_enabled\":");
    response += cloud.deepseekEnabled ? F("true") : F("false");
    response += F(",\"deepseek_api_key_set\":");
    response += cloud.deepseekApiKey[0] != '\0' ? F("true") : F("false");
    response += F(",\"deepseek_api_url\":\"");
    response += jsonEscape(cloud.deepseekApiUrl);
    response += F("\",\"deepseek_model\":\"");
    response += jsonEscape(cloud.deepseekModel);
    response += F("\"},\"profile\":{\"nickname\":\"");
    response += jsonEscape(profile.nickname);
    response += F("\",\"function_test_enabled\":");
    response += profile.functionTestEnabled ? F("true") : F("false");
    response += F(",\"ai_scene_test_enabled\":");
    response += profile.aiSceneTestEnabled ? F("true") : F("false");
    response += F(",\"test_scene\":\"");
    response += function_area::sceneId(profile.testScene);
    response += F("\"}}");
    sendJson(server, 200, response);
  });

  server.on("/api/networks", HTTP_GET, [&server]() {
    const int count = WiFi.scanNetworks(false, true);
    String response = "[";
    for (int i = 0; i < count; ++i) {
      if (i > 0) response += ',';
      response += F("{\"s\":\"");
      response += jsonEscape(WiFi.SSID(i));
      response += F("\",\"r\":");
      response += WiFi.RSSI(i);
      response += '}';
    }
    response += ']';
    WiFi.scanDelete();
    sendJson(server, 200, response);
  });

  server.on("/api/tasks", HTTP_GET, [&server]() {
    String response;
    response.reserve(15360);
    response += F("{\"storage_ready\":");
    response += task_store::ready() ? F("true") : F("false");
    response += F(",\"active\":[");
    for (uint8_t region = 0; region < task_store::kRegionCount; ++region) {
      if (region > 0) response += ',';
      task_store::CurrentTask task = {};
      task_store::getCurrent(region, task);
      response += F("{\"region\":");
      response += region + 1;
      response += F(",\"present\":");
      response += task.present ? F("true") : F("false");
      response += F(",\"completed\":");
      response += task.completed ? F("true") : F("false");
      response += F(",\"history_eligible\":");
      response += task.historyEligible ? F("true") : F("false");
      response += F(",\"revision\":");
      response += task.revision;
      response += F(",\"schedule_kind\":\"");
      response += task.schedule.kind == task_store::ScheduleKind::Exact
                      ? F("exact")
                  : task.schedule.kind == task_store::ScheduleKind::Window
                      ? F("window")
                      : F("none");
      response += F("\",\"start_at\":");
      appendUint64(response, task.schedule.startAt);
      response += F(",\"end_at\":");
      appendUint64(response, task.schedule.endAt);
      response += F(",\"due_soon_sent\":");
      response += task.dueSoonSent ? F("true") : F("false");
      response += F(",\"due_check_sent\":");
      response += task.dueCheckSent ? F("true") : F("false");
      response += F(",\"text\":\"");
      response += jsonEscape(task.text);
      response += F("\"}");
    }
    response += F("],\"completed\":[");
    const size_t count = task_store::completedCount();
    bool firstCompleted = true;
    for (size_t index = 0; index < count; ++index) {
      task_store::CompletedTask task = {};
      if (!task_store::getCompleted(index, task)) continue;
      if (!firstCompleted) response += ',';
      firstCompleted = false;
      response += F("{\"id\":");
      response += task.sequence;
      response += F(",\"region\":");
      response += task.region + 1;
      response += F(",\"completed_at\":");
      appendUint64(response, task.completedAt);
      response += F(",\"text\":\"");
      response += jsonEscape(task.text);
      response += F("\"}");
    }
    response += F("]}");
    sendJson(server, 200, response);
  });
}

void registerConfigurationRoutes(WebServer& server) {
  server.on("/api/wifi", HTTP_POST, [&server]() {
    String ssid = server.arg("ssid");
    const String password = server.arg("password");
    ssid.trim();
    if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 63 ||
        (!password.isEmpty() && password.length() < 8)) {
      sendMessage(server, 400, F("Wi-Fi 信息格式无效"));
      return;
    }

    Serial.printf("Testing submitted Wi-Fi: %s\n", ssid.c_str());
    WiFi.begin(ssid.c_str(), password.c_str());
    const uint32_t started = millis();
    while (WiFi.status() != WL_CONNECTED &&
           millis() - started < kConnectTimeoutMs) {
      delay(250);
    }

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("ERROR: Submitted Wi-Fi credentials did not connect.");
      WiFi.disconnect(false, false);
      if (!savedSsid.isEmpty()) {
        WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
      }
      sendMessage(server, 503, F("连接失败，请检查名称、密码和 2.4 GHz 频段"));
      return;
    }

    if (!saveCredentials(ssid, password)) {
      Serial.println("ERROR: Could not save Wi-Fi credentials.");
      sendMessage(server, 500, F("保存 Wi-Fi 信息失败"));
      return;
    }

    Serial.println("Wi-Fi credentials verified and saved; restarting.");
    sendMessage(server, 200, F("网络已保存，设备即将重新启动"));
    delay(900);
    ESP.restart();
  });

  server.on("/api/wifi/forget", HTTP_POST, [&server]() {
    if (savedSsid.isEmpty()) {
      sendMessage(server, 200, F("当前没有已保存的 Wi-Fi"));
      return;
    }
    if (!clearCredentials()) {
      sendMessage(server, 500, F("清除 Wi-Fi 信息失败"));
      return;
    }
    WiFi.disconnect(false, false);
    sendMessage(server, 200, F("已忘记当前 Wi-Fi"));
  });

  server.on("/api/settings", HTTP_POST, [&server]() {
    String host = server.arg("host");
    String path = server.arg("path");
    const long port = server.arg("port").toInt();
    host.trim();
    path.trim();
    if (port <= 0 || port > 65535 ||
        !device_settings::saveReceiver(host, static_cast<uint16_t>(port),
                                       path)) {
      sendMessage(server, 400, F("识别服务地址格式无效"));
      return;
    }
    sendMessage(server, 200, F("识别服务设置已保存"));
  });

  server.on("/api/settings/reset", HTTP_POST, [&server]() {
    device_settings::resetReceiver();
    sendMessage(server, 200, F("已恢复编译时默认设置"));
  });

  server.on("/api/profile", HTTP_POST, [&server]() {
    String nickname = server.arg("nickname");
    String sceneId = server.arg("test_scene");
    nickname.trim();
    sceneId.trim();
    function_area::Scene scene = function_area::Scene::Welcome;
    if (!function_area::sceneFromId(sceneId.c_str(), scene) ||
        !device_settings::saveProfile(
            nickname, server.arg("function_test_enabled") == "1",
            server.arg("ai_scene_test_enabled") == "1", scene)) {
      sendMessage(server, 400, F("称呼或功能区测试设置格式无效"));
      return;
    }
    sendMessage(server, 200, F("称呼和功能区设置已保存"));
  });

  server.on("/api/cloud", HTTP_POST, [&server]() {
    device_settings::CloudSettingsUpdate update;
    update.deepseekEnabled = server.arg("deepseek_enabled") == "1";
    update.baiduApiKey = server.arg("baidu_api_key");
    update.baiduSecretKey = server.arg("baidu_secret_key");
    update.baiduSpeechUrl = server.arg("baidu_speech_url");
    update.deepseekApiKey = server.arg("deepseek_api_key");
    update.deepseekApiUrl = server.arg("deepseek_api_url");
    update.deepseekModel = server.arg("deepseek_model");
    if (!device_settings::saveCloud(update, true)) {
      sendMessage(server, 400, F("云 API 配置格式无效"));
      return;
    }
    sendMessage(server, 200, F("云 API 配置已保存"));
  });

  server.on("/api/cloud/reset", HTTP_POST, [&server]() {
    device_settings::resetCloud();
    sendMessage(server, 200, F("云 API 配置已清除"));
  });

  server.on("/api/tasks/history/clear", HTTP_POST, [&server]() {
    if (!task_store::clearCompleted()) {
      sendMessage(server, 500, F("清除完成记录失败"));
      return;
    }
    sendMessage(server, 200, F("完成记录已清除"));
  });
}

void registerSystemRoutes(WebServer& server) {
  server.on("/api/restart", HTTP_POST, [&server]() {
    sendMessage(server, 200, F("设备正在重新启动"));
    delay(600);
    ESP.restart();
  });

  server.on("/api/exit", HTTP_POST, [&server]() {
    if (!captivePortalMode) {
      sendMessage(server, 409, F("设备已处于正常工作模式"));
      return;
    }
    if (savedSsid.isEmpty()) {
      sendMessage(server, 409, F("请先配置 Wi-Fi"));
      return;
    }
    sendMessage(server, 200, F("正在退出管理模式"));
    delay(600);
    ESP.restart();
  });
}

void startAdminServer() {
  if (adminServerStarted) return;

  adminServer.on("/", HTTP_GET,
                 []() { sendAdminPage(adminServer); });
  registerStatusRoutes(adminServer);
  registerConfigurationRoutes(adminServer);
  registerSystemRoutes(adminServer);
  adminServer.on("/generate_204", HTTP_ANY,
                 []() { sendAdminPage(adminServer); });
  adminServer.on("/hotspot-detect.html", HTTP_ANY,
                 []() { sendAdminPage(adminServer); });
  adminServer.on("/connecttest.txt", HTTP_ANY,
                 []() { sendAdminPage(adminServer); });
  adminServer.on("/ncsi.txt", HTTP_ANY,
                 []() { sendAdminPage(adminServer); });
  adminServer.onNotFound([]() { sendNotFound(adminServer); });
  adminServer.begin();
  adminServerStarted = true;
  Serial.println("Management HTTP server ready.");
}

bool startPortal(PortalStartedCallback portalStarted) {
  captivePortalMode = true;
  const bool stationWasConnected = WiFi.status() == WL_CONNECTED;
  if (!stationWasConnected) WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  if (!WiFi.softAP(kSetupApSsid, kSetupApPassword)) {
    Serial.println("ERROR: Could not start the management access point.");
    return false;
  }

  if (portalStarted != nullptr) portalStarted();

  const IPAddress portalIp = WiFi.softAPIP();
  Serial.printf("Management AP: %s\n", kSetupApSsid);
  Serial.printf("Management password: %s\n", kSetupApPassword);
  Serial.print("Management page: http://");
  Serial.println(portalIp);

  adminDnsServer.start(kDnsPort, "*", portalIp);
  startAdminServer();

  Serial.println("Management portal ready.");
  while (true) {
    adminDnsServer.processNextRequest();
    adminServer.handleClient();
    delay(5);
  }
  return false;
}

}  // namespace

bool hasSavedCredentials() {
  loadCredentials();
  return !savedSsid.isEmpty();
}

bool begin(bool forcePortal, PortalStartedCallback portalStarted) {
  device_settings::begin();
  loadCredentials();
  if (forcePortal) {
    Serial.println("Explicit management mode requested.");
    return startPortal(portalStarted);
  }

  if (!savedSsid.isEmpty() && connectStation(savedSsid, savedPassword)) {
    captivePortalMode = false;
    startAdminServer();
    return true;
  }

  return startPortal(portalStarted);
}

bool reconnect() {
  if (WiFi.status() == WL_CONNECTED) return true;
  loadCredentials();
  return connectStation(savedSsid, savedPassword);
}

void poll() {
  if (captivePortalMode || !adminServerStarted) return;
  adminServer.handleClient();
}

}  // namespace wifi_provisioning
