#include "SoccerCommon.h"

static String myName = "Soccer-server";

struct Seen { String name; uint32_t ms; };
Seen seen[32];

void remember(const char* n) {
  if (!n || !*n) return;
  for (int i=0;i<32;i++){
    if (seen[i].name.equalsIgnoreCase(n)) { seen[i].ms = millis(); return; }
  }
  for (int i=0;i<32;i++){
    if (seen[i].name.length()==0){ seen[i].name = n; seen[i].ms = millis(); return; }
  }
}

void onNowRecv(const esp_now_recv_info* info, const uint8_t* data, int len) {
  if (len < (int)sizeof(MsgHeader)) return;
  const MsgHeader* h = (const MsgHeader*)data;
  if (h->kind == MSG_HELLO && len >= (int)sizeof(HelloMsg)) {
    const HelloMsg* m = (const HelloMsg*)data;
    remember(m->name);
  }
}

// ---------- Send helpers ----------
void sendWifiSet(const char* target, const String& ssid, const String& pass) {
  WifiSetMsg m{}; m.h.kind = MSG_WIFI_SET;
  strncpy(m.h.target, target, sizeof(m.h.target)-1);
  strncpy(m.ssid, ssid.c_str(), sizeof(m.ssid)-1);
  strncpy(m.pass, pass.c_str(), sizeof(m.pass)-1);
  esp_now_send(BCAST, (uint8_t*)&m, sizeof(m));
}

void sendOta(const char* target, const String& url) {
  OtaMsg m{}; m.h.kind = MSG_OTA_TRIGGER;
  strncpy(m.h.target, target, sizeof(m.h.target)-1);
  strncpy(m.url, url.c_str(), sizeof(m.url)-1);
  esp_now_send(BCAST, (uint8_t*)&m, sizeof(m));
}

void sendNameSet(const char* target, const String& newName) {
  NameSetMsg m{}; m.h.kind = MSG_NAME_SET;
  strncpy(m.h.target, target, sizeof(m.h.target)-1);
  strncpy(m.name, newName.c_str(), sizeof(m.name)-1);
  esp_now_send(BCAST, (uint8_t*)&m, sizeof(m));
}

void sendHelloReqAll() {
  MsgHeader h{}; h.kind = MSG_HELLO_REQ;
  strncpy(h.target, "ALL", sizeof(h.target)-1);
  esp_now_send(BCAST, (uint8_t*)&h, sizeof(h));
}

// ---------- “Drip” HELLO_REQ state machine ----------
static bool     helloDripActive   = false;
static uint8_t  helloDripRemain   = 0;
static uint32_t helloNextAt       = 0;
static uint32_t listPrintAt       = 0;

// tune as you like
static const uint8_t  HELLO_DRIP_COUNT        = 5;      // how many HELLO_REQ to send
static const uint32_t HELLO_DRIP_INTERVAL_MS  = 200;    // spacing between them
static const uint32_t HELLO_SETTLE_MS         = 350;    // wait after last drip before printing

// ---------- CLI ----------
void printHelp() {
  Serial.println();
  Serial.println("=== Soccer Server CLI ===");
  Serial.println("WIFI ALL <ssid> <pass>           - set Wi-Fi for all clients");
  Serial.println("WIFI <name> <ssid> <pass>        - set Wi-Fi for one client");
  Serial.println("OTA  ALL <url>                   - trigger HTTP OTA for all");
  Serial.println("OTA  <name> <url>                - trigger HTTP OTA for one");
  Serial.println("NAME <ALL|currentName> <newName> - rename client(s) (persists in NVS)");
  Serial.println("LIST                             - drip HELLO requests, then print roster");
  Serial.println("HELP                             - show this help");
  Serial.println();
  Serial.println("Examples:");
  Serial.println("  WIFI ALL AndrewiPhone 12345678");
  Serial.println("  WIFI Soccer-topright AndrewiPhone 12345678");
  Serial.println("  NAME Soccer-topright Soccer-topleft");
  Serial.println("  OTA  ALL http://172.20.10.2:8000/Soccer/SoccerClient/build/esp32.esp32.esp32/SoccerClient.ino.bin");
  Serial.println("  OTA  Soccer-side http://172.20.10.2:8000/Soccer/SoccerClient/build/esp32.esp32.esp32/SoccerClient.ino.bin");
  Serial.println("===========================");
  Serial.println();
}

void handleSerialServer() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n'); line.trim();
  if (!line.length()) return;

  int sp1 = line.indexOf(' ');
  String cmd = (sp1<0)? line : line.substring(0,sp1);
  cmd.toUpperCase();

  if (cmd == "HELP") { printHelp(); return; }

  if (cmd == "LIST") {
    // kick off a drip of HELLO_REQ in loop
    helloDripActive = true;
    helloDripRemain = HELLO_DRIP_COUNT;
    helloNextAt     = millis();          // send first on next loop tick
    listPrintAt     = 0;                 // will be set when drip completes
    Serial.println("Requesting HELLOs (drip)...");
    return;
  }

  if (cmd == "WIFI") {
    int sp2 = line.indexOf(' ', sp1+1); if (sp2<0){ Serial.println("Usage: WIFI <ALL|name> <ssid> <pass>"); return; }
    int sp3 = line.indexOf(' ', sp2+1); if (sp3<0){ Serial.println("Usage: WIFI <ALL|name> <ssid> <pass>"); return; }
    String target = line.substring(sp1+1, sp2);
    String ssid   = line.substring(sp2+1, sp3);
    String pass   = line.substring(sp3+1);
    sendWifiSet(target.c_str(), ssid, pass);
    Serial.printf("Sent WIFI_SET to %s (ssid=%s)\n", target.c_str(), ssid.c_str());
    return;
  }

  if (cmd == "OTA") {
    int sp2 = line.indexOf(' ', sp1+1); if (sp2<0){ Serial.println("Usage: OTA <ALL|name> <url>"); return; }
    String target = line.substring(sp1+1, sp2);
    String url    = line.substring(sp2+1);
    if (!url.length()) { Serial.println("Usage: OTA <ALL|name> <url>"); return; }
    sendOta(target.c_str(), url);
    Serial.printf("Sent OTA_TRIGGER to %s\n", target.c_str());
    return;
  }

  if (cmd == "NAME") {
    // NAME <ALL|currentName> <newName>
    int sp2 = line.indexOf(' ', sp1+1); if (sp2<0){ Serial.println("Usage: NAME <ALL|currentName> <newName>"); return; }
    String target  = line.substring(sp1+1, sp2);
    String newName = line.substring(sp2+1); newName.trim();
    if (!newName.length()) { Serial.println("Usage: NAME <ALL|currentName> <newName>"); return; }
    sendNameSet(target.c_str(), newName);
    Serial.printf("Sent NAME_SET to %s -> %s\n", target.c_str(), newName.c_str());
    return;
  }

  Serial.println("Unknown command. Type HELP.");
}

void setup() {
  Serial.begin(115200); delay(200);
  nvsSaveName(myName);

  wifiStaOnCh6();
  if (!nowInit()) { Serial.println("ESP-NOW init failed!"); }
  esp_now_register_recv_cb(onNowRecv);

  // Announce server presence (optional)
  HelloMsg hm{}; hm.h.kind = MSG_HELLO;
  strncpy(hm.h.target, "ALL", sizeof(hm.h.target)-1);
  strncpy(hm.name, myName.c_str(), sizeof(hm.name)-1);
  esp_now_send(BCAST, (uint8_t*)&hm, sizeof(hm));

  Serial.println("Server ready. Type HELP for commands.");
}

void loop() {
  handleSerialServer();

  // Drip scheduler
  uint32_t now = millis();
  if (helloDripActive) {
    if (helloDripRemain && (int32_t)(now - helloNextAt) >= 0) {
      sendHelloReqAll();
      helloDripRemain--;
      helloNextAt = now + HELLO_DRIP_INTERVAL_MS;

      // when last one goes out, schedule the print window
      if (helloDripRemain == 0) {
        helloDripActive = false;
        listPrintAt = helloNextAt + HELLO_SETTLE_MS; // a little time for replies to arrive
      }
    }
  }

  // Print roster after drip completes + settle time
  if (listPrintAt && (int32_t)(now - listPrintAt) >= 0) {
    listPrintAt = 0;
    Serial.println("Last seen clients:");
    for (int i=0;i<32;i++){
      if (seen[i].name.length())
        Serial.printf(" - %s  (%lus ago)\n", seen[i].name.c_str(),
                      (unsigned long)((now - seen[i].ms)/1000));
    }
  }

  delay(2);
}
