#include "wifi_provisioning.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <cstring>

namespace wifi_provisioning {
namespace {

constexpr char kPreferencesNamespace[] = "eink-wifi";
constexpr char kCredentialsKey[] = "credentials";
constexpr uint32_t kCredentialsMagic = 0x57494649U;  // "WIFI"
constexpr uint8_t kCredentialsVersion = 1;
constexpr uint32_t kConnectTimeoutMs = 20000;
constexpr uint16_t kDnsPort = 53;
constexpr uint16_t kHttpPort = 80;

static const char kPortalHtml[] PROGMEM = R"html(
<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>墨水屏 Wi-Fi 配置</title>
  <style>
    *{box-sizing:border-box}body{margin:0;font-family:system-ui,sans-serif;color:#171717;background:#f4f5f7}
    main{width:min(100%,440px);margin:0 auto;padding:28px 20px}h1{font-size:24px;margin:0 0 8px}
    p{margin:0 0 20px;color:#555;line-height:1.5}label{display:block;margin:16px 0 6px;font-weight:600}
    input{width:100%;height:44px;border:1px solid #aaa;background:#fff;padding:0 12px;font-size:16px;border-radius:4px}
    button{height:44px;border:0;border-radius:4px;background:#111;color:#fff;font-size:16px;font-weight:600;cursor:pointer}
    form>button{width:100%;margin-top:20px}.network{width:100%;display:flex;justify-content:space-between;align-items:center;
    margin:0 0 8px;padding:0 12px;background:#fff;color:#171717;border:1px solid #bbb;font-weight:400;text-align:left}
    .network span:last-child{color:#666;font-size:13px}#networks{min-height:44px}.status{font-size:14px;color:#666}
  </style>
</head>
<body><main>
  <h1>连接 Wi-Fi</h1>
  <p>选择 2.4 GHz 网络并输入密码，设备验证成功后会自动重启。</p>
  <div id="networks" class="status">正在扫描网络...</div>
  <form action="/save" method="post">
    <label for="ssid">网络名称</label>
    <input id="ssid" name="ssid" maxlength="32" required autocomplete="off">
    <label for="password">密码</label>
    <input id="password" name="password" type="password" maxlength="63" autocomplete="current-password">
    <button type="submit">连接</button>
  </form>
</main>
<script>
fetch('/scan').then(r=>r.json()).then(items=>{
  const box=document.getElementById('networks');box.textContent='';
  if(!items.length){box.textContent='未发现网络，可以手动输入名称。';return;}
  items.forEach(item=>{const button=document.createElement('button');button.type='button';button.className='network';
    const name=document.createElement('span');name.textContent=item.s;const signal=document.createElement('span');
    signal.textContent=item.r+' dBm';button.append(name,signal);button.addEventListener('click',()=>{
      document.getElementById('ssid').value=item.s;document.getElementById('password').focus();});box.append(button);});
}).catch(()=>{document.getElementById('networks').textContent='扫描失败，可以手动输入网络名称。';});
</script></body></html>
)html";

String savedSsid;
String savedPassword;

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

void sendPortalRedirect(WebServer& server) {
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(),
                    true);
  server.send(302, "text/plain", "");
}

bool startPortal(PortalStartedCallback portalStarted) {
  WiFi.disconnect(false, false);
  WiFi.mode(WIFI_AP_STA);
  if (!WiFi.softAP(kSetupApSsid, kSetupApPassword)) {
    Serial.println("ERROR: Could not start the Wi-Fi setup access point.");
    return false;
  }

  if (portalStarted != nullptr) portalStarted();

  const IPAddress portalIp = WiFi.softAPIP();
  Serial.printf("Wi-Fi setup AP: %s\n", kSetupApSsid);
  Serial.printf("Wi-Fi setup password: %s\n", kSetupApPassword);
  Serial.print("Wi-Fi setup page: http://");
  Serial.println(portalIp);

  DNSServer dnsServer;
  dnsServer.start(kDnsPort, "*", portalIp);
  WebServer server(kHttpPort);

  server.on("/", HTTP_GET,
            [&server]() { server.send_P(200, "text/html", kPortalHtml); });
  server.on("/scan", HTTP_GET, [&server]() {
    const int count = WiFi.scanNetworks();
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
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", response);
  });
  server.on("/save", HTTP_POST, [&server]() {
    String ssid = server.arg("ssid");
    const String password = server.arg("password");
    ssid.trim();
    if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 63 ||
        (!password.isEmpty() && password.length() < 8)) {
      server.send(400, "text/plain; charset=utf-8", "Wi-Fi 信息格式无效");
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
      server.send(503, "text/html; charset=utf-8",
                  "<meta name='viewport' content='width=device-width'>"
                  "<body style='font-family:sans-serif;padding:32px'>"
                  "<h2>连接失败</h2><p>请检查网络名称、密码和 2.4 GHz 频段。</p>"
                  "<p><a href='/'>返回重新配置</a></p></body>");
      return;
    }

    if (!saveCredentials(ssid, password)) {
      Serial.println("ERROR: Could not save Wi-Fi credentials.");
      server.send(500, "text/plain; charset=utf-8", "保存 Wi-Fi 信息失败");
      return;
    }

    Serial.println("Wi-Fi credentials verified and saved; restarting.");
    server.send(200, "text/html; charset=utf-8",
                "<meta name='viewport' content='width=device-width'>"
                "<body style='font-family:sans-serif;padding:32px'>"
                "<h2>连接成功</h2><p>设备即将重新启动。</p></body>");
    delay(1500);
    ESP.restart();
  });

  server.on("/generate_204", HTTP_ANY,
            [&server]() { sendPortalRedirect(server); });
  server.on("/hotspot-detect.html", HTTP_ANY,
            [&server]() { sendPortalRedirect(server); });
  server.on("/connecttest.txt", HTTP_ANY,
            [&server]() { sendPortalRedirect(server); });
  server.on("/ncsi.txt", HTTP_ANY,
            [&server]() { sendPortalRedirect(server); });
  server.onNotFound([&server]() { sendPortalRedirect(server); });
  server.begin();

  while (true) {
    dnsServer.processNextRequest();
    server.handleClient();
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
  loadCredentials();
  if (forcePortal) {
    Serial.println("Wi-Fi setup requested by the A1+A2 hold gesture.");
    return startPortal(portalStarted);
  }

  if (!savedSsid.isEmpty() && connectStation(savedSsid, savedPassword)) {
    return true;
  }

  return startPortal(portalStarted);
}

bool reconnect() {
  if (WiFi.status() == WL_CONNECTED) return true;
  loadCredentials();
  return connectStation(savedSsid, savedPassword);
}

}  // namespace wifi_provisioning
