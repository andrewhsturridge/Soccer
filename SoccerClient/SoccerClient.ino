#include "SoccerCommon.h"

// ========== EDIT PER CLIENT ON FIRST FLASH ==========
#define HOSTNAME "Soccer-topleft"
// ====================================================

static String deviceName = HOSTNAME;

static bool espNowStarted = false;
static void startEspNow() {
  if (espNowStarted) return;
  wifiStaOnCh6();
  if (nowInit()) {
    esp_now_register_recv_cb(nullptr); // we’ll set it after start
    espNowStarted = true;
  }
}
static void stopEspNow() {
  if (!espNowStarted) return;
  esp_now_deinit();
  espNowStarted = false;
}
static inline void sendHello() {
  HelloMsg hm{}; hm.h.kind = MSG_HELLO;
  strncpy(hm.h.target, "ALL", sizeof(hm.h.target)-1);
  strncpy(hm.name, deviceName.c_str(), sizeof(hm.name)-1);
  esp_now_send(BCAST, (uint8_t*)&hm, sizeof(hm));
}

// ---------- Pending request queues (callback -> loop) ----------
static char qWifiSsid[32] = {0};
static char qWifiPass[64] = {0};
static volatile bool qWifiPending = false;

static char qOtaUrl[192] = {0};
static volatile bool qOtaPending = false;

static inline void queueWifiSet(const char* ssid, const char* pass) {
  // copy payload first
  if (ssid) { strncpy(qWifiSsid, ssid, sizeof(qWifiSsid)-1); qWifiSsid[sizeof(qWifiSsid)-1]=0; }
  else qWifiSsid[0]=0;
  if (pass) { strncpy(qWifiPass, pass, sizeof(qWifiPass)-1); qWifiPass[sizeof(qWifiPass)-1]=0; }
  else qWifiPass[0]=0;
  // publish flag last
  qWifiPending = true;
}

static inline void queueOta(const char* url) {
  if (url) { strncpy(qOtaUrl, url, sizeof(qOtaUrl)-1); qOtaUrl[sizeof(qOtaUrl)-1]=0; }
  else qOtaUrl[0]=0;
  qOtaPending = true;
}

// ---------- Wi-Fi connect helper for OTA ----------
static bool connectForOta(String* failWhy = nullptr) {
  String ssid, pass; nvsLoadWifi(ssid, pass);

  // Clean slate
  WiFi.disconnect(true, true);
  delay(50);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname(deviceName.c_str());

  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  wl_status_t st;
  while ((st = WiFi.status()) != WL_CONNECTED && millis() - t0 < 15000) {
    delay(100);
  }
  if (st == WL_CONNECTED) return true;
  if (failWhy) *failWhy = String("status=") + String((int)st) +
                          " ssid=" + ssid + " ip=" + WiFi.localIP().toString();
  return false;
}

// ---------- ESP-NOW RX callback: ONLY queue work ----------
static void onNowRecv(const esp_now_recv_info* info, const uint8_t* data, int len) {
  if (len < (int)sizeof(MsgHeader)) return;
  const MsgHeader* h = (const MsgHeader*)data;

  if (h->kind == MSG_WIFI_SET && len >= (int)sizeof(WifiSetMsg)) {
    const WifiSetMsg* m = (const WifiSetMsg*)data;
    if (!nameMatches(m->h.target, deviceName)) return;
    queueWifiSet(m->ssid, m->pass);
    return;
  }

  if (h->kind == MSG_OTA_TRIGGER && len >= (int)sizeof(OtaMsg)) {
    const OtaMsg* m = (const OtaMsg*)data;
    if (!nameMatches(m->h.target, deviceName)) return;
    if (m->url[0] == 0) return;
    queueOta(m->url);
    return;
  }

  if (h->kind == MSG_NAME_SET && len >= (int)sizeof(NameSetMsg)) {
    const NameSetMsg* m = (const NameSetMsg*)data;
    if (!nameMatches(m->h.target, deviceName)) return;
    String newName = String(m->name); newName.trim();
    if (newName.length()) {
      nvsSaveName(newName);
      deviceName = newName;               // update RAM copy for immediate HELLO
      Serial.printf("[NAME] Set to %s, rebooting...\n", deviceName.c_str());
      delay(150);
      ESP.restart();
    }
    return;
  }

  if (h->kind == MSG_HELLO_REQ) {
    // Reply immediately with a HELLO (no reboots or other work)
    sendHello();
    return;
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  String saved = nvsLoadName();
  if (saved.length()) deviceName = saved;
  else nvsSaveName(deviceName);

  startEspNow();
  esp_now_register_recv_cb(onNowRecv);

  // HELLO announce (so server LIST sees us)
  sendHello();

  Serial.printf("Boot %s. ESP-NOW active on ch%d. Waiting for server.\n",
                deviceName.c_str(), ESPNOW_CHANNEL);
}

void loop() {
  // Handle queued WIFI_SET (persist + reboot) in the loop
  if (qWifiPending) {
    qWifiPending = false;            // take ownership
    nvsSaveWifi(String(qWifiSsid), String(qWifiPass));
    Serial.printf("[WIFI] Saved new SSID=%s, rebooting...\n", qWifiSsid);
    delay(150);
    ESP.restart();
  }

  // Handle queued OTA (stop-now → Wi-Fi → update) in the loop
  if (qOtaPending) {
    qOtaPending = false;             // take ownership
    String url = String(qOtaUrl);
    Serial.printf("[OTA] Switching to Wi-Fi for: %s\n", url.c_str());

    stopEspNow();
    delay(50);

    String why;
    if (connectForOta(&why)) {
      Serial.printf("[OTA] Wi-Fi connected: IP=%s\n", WiFi.localIP().toString().c_str());
      WiFiClient client;
      HTTPUpdate httpUpdate; httpUpdate.rebootOnUpdate(true);
      t_httpUpdate_return r = httpUpdate.update(client, url);
      if (r != HTTP_UPDATE_OK) {
        Serial.printf("[OTA] Update failed: %s\n", httpUpdate.getLastErrorString().c_str());
        // restore ESP-NOW if no reboot occurred
        wifiStaOnCh6();
        startEspNow();
      }
      // success path reboots automatically
    } else {
      Serial.printf("[OTA] Wi-Fi connect failed (%s). Restoring ESP-NOW.\n", why.c_str());
      wifiStaOnCh6();
      startEspNow();
    }
  }

  // Your normal ESP-NOW game loop here
  delay(5);
}
