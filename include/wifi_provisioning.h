#pragma once

namespace wifi_provisioning {

constexpr char kSetupApSsid[] = "EinkTask-Setup";
constexpr char kSetupApPassword[] = "12345678";

using PortalStartedCallback = void (*)();

bool hasSavedCredentials();
bool begin(bool forcePortal, PortalStartedCallback portalStarted = nullptr);
bool reconnect();
void poll();

}  // namespace wifi_provisioning
