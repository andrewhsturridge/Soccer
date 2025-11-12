#include "SoccerCommon.h"

static String myName = "Soccer-server";

// Roster
struct Seen { String name; uint32_t ms; };
Seen seen[32];
void remember(const char* n){
  if(!n||!*n) return;
  for(int i=0;i<32;i++) if (seen[i].name.equalsIgnoreCase(n)){ seen[i].ms=millis(); return; }
  for(int i=0;i<32;i++) if (!seen[i].name.length()){ seen[i].name=n; seen[i].ms=millis(); return; }
}

// Names
static const char* NAME_TOPLEFT  = "Soccer-topleft";
static const char* NAME_TOPRIGHT = "Soccer-topright";
static const char* NAME_SIDE     = "Soccer-side";

// Overlay flash windows (server-driven)
static const uint32_t FLASH_MS = 300;    // how long to show red per trigger
static uint32_t topUntil[24]  = {0};     // 1..23 used
static uint32_t sideUntil[14] = {0};     // 1..13 used

// HELLO drip
static bool helloDripActive=false; static uint8_t helloDripRemain=0;
static uint32_t helloNextAt=0, listPrintAt=0;
static const uint8_t  HELLO_DRIP_COUNT=5;
static const uint32_t HELLO_DRIP_INTERVAL_MS=200, HELLO_SETTLE_MS=350;

// LED push cadence
static uint32_t nextLedPushAt=0;
static const uint32_t LED_PUSH_MIN_MS=40;   // ~25 fps cap

// Build bitsets from expiry tables
static inline void buildTopBits(uint8_t bits[3]){
  memset(bits,0,3); uint32_t now=millis();
  for (int id=1; id<=23; ++id) if (now < topUntil[id]) { int b=(id-1)>>3, k=(id-1)&7; bits[b] |= (1<<k); }
}
static inline void buildSideBits(uint8_t bits[2]){
  memset(bits,0,2); uint32_t now=millis();
  for (int id=1; id<=13; ++id) if (now < sideUntil[id]){ int b=(id-1)>>3, k=(id-1)&7; bits[b] |= (1<<k); }
}

// Sends
static void sendHelloReqAll(){ MsgHeader h{}; h.kind=MSG_HELLO_REQ; strncpy(h.target,"ALL",sizeof(h.target)-1); esp_now_send(BCAST,(uint8_t*)&h,sizeof(h)); }
static void sendWifiSet(const char* target,const String& ssid,const String& pass){ WifiSetMsg m{}; m.h.kind=MSG_WIFI_SET; strncpy(m.h.target,target,sizeof(m.h.target)-1); strncpy(m.ssid,ssid.c_str(),sizeof(m.ssid)-1); strncpy(m.pass,pass.c_str(),sizeof(m.pass)-1); esp_now_send(BCAST,(uint8_t*)&m,sizeof(m)); }
static void sendOta(const char* target,const String& url){ OtaMsg m{}; m.h.kind=MSG_OTA_TRIGGER; const char* tgt=(target&&*target)?target:"ALL"; strncpy(m.h.target,tgt,sizeof(m.h.target)-1); strncpy(m.url,url.c_str(),sizeof(m.url)-1); esp_now_send(BCAST,(uint8_t*)&m,sizeof(m)); }
static void sendNameSet(const char* target,const String& newName){ NameSetMsg m{}; m.h.kind=MSG_NAME_SET; strncpy(m.h.target,target,sizeof(m.h.target)-1); strncpy(m.name,newName.c_str(),sizeof(m.name)-1); esp_now_send(BCAST,(uint8_t*)&m,sizeof(m)); }

static void pushLedStates(){
  uint8_t bTop[3];  buildTopBits(bTop);
  uint8_t bSide[2]; buildSideBits(bSide);

  { LedTopMsg t{};  t.h.kind=MSG_LED_TOP;  strncpy(t.h.target,NAME_TOPRIGHT,sizeof(t.h.target)-1); memcpy(t.bits,bTop,sizeof(bTop));   esp_now_send(BCAST,(uint8_t*)&t,sizeof(t)); }
  { LedSideMsg s{}; s.h.kind=MSG_LED_SIDE; strncpy(s.h.target,NAME_SIDE,    sizeof(s.h.target)-1); memcpy(s.bits,bSide,sizeof(bSide)); esp_now_send(BCAST,(uint8_t*)&s,sizeof(s)); }
}

// RX
static void onNowRecv(const esp_now_recv_info* info, const uint8_t* data, int len){
  if (len < (int)sizeof(MsgHeader)) return;
  const MsgHeader* h=(const MsgHeader*)data;

  if (h->kind == MSG_HELLO && len >= (int)sizeof(HelloMsg)){
    const HelloMsg* m=(const HelloMsg*)data; remember(m->name);
    // Optionally wipe expiries for that device on HELLO
    uint32_t now = millis();
    if (!strcasecmp(m->name, NAME_TOPRIGHT) || !strcasecmp(m->name, NAME_TOPLEFT)) {
      for (int i=1;i<=23;i++) topUntil[i]=now; // expire immediately
    } else if (!strcasecmp(m->name, NAME_SIDE)) {
      for (int i=1;i<=13;i++) sideUntil[i]=now;
    }
    if ((int32_t)(now - nextLedPushAt) > 0) nextLedPushAt = now;
    return;
  }

  if (h->kind == MSG_SENSOR_EVENT && len >= (int)sizeof(SensorEventMsg)){
    const SensorEventMsg* m=(const SensorEventMsg*)data; remember(m->name);

    bool isTopL  = !strcasecmp(m->name, NAME_TOPLEFT);
    bool isTopR  = !strcasecmp(m->name, NAME_TOPRIGHT);
    bool isSide  = !strcasecmp(m->name, NAME_SIDE);

    // We only care that a TRIGGER happened; ignore state=0
    if (m->state == 1){
      uint32_t until = millis() + FLASH_MS;
      if (isTopL || isTopR){
        if (m->sensor_id >=1 && m->sensor_id <= 23) topUntil[m->sensor_id] = until;
      } else if (isSide){
        if (m->sensor_id >=1 && m->sensor_id <= 13) sideUntil[m->sensor_id] = until;
      }
      if ((int32_t)(millis() - nextLedPushAt) > 0) nextLedPushAt = millis();
    }

    // DEBUG
    Serial.printf("[EVT] %s s%u gpio%u state=%u seq=%u\n", m->name, m->sensor_id, m->gpio, m->state, m->seq);
    return;
  }
}

// CLI
static void printHelp(){
  Serial.println();
  Serial.println("=== Soccer Server CLI ===");
  Serial.println("WIFI ALL <ssid> <pass>");
  Serial.println("WIFI <name> <ssid> <pass>");
  Serial.println("OTA  ALL <url>");
  Serial.println("OTA  <name> <url>");
  Serial.println("NAME <ALL|currentName> <newName>");
  Serial.println("LIST");
  Serial.println();
  Serial.println("Examples:");
  Serial.println("  WIFI ALL AndrewiPhone 12345678");
  Serial.println("  OTA  ALL http://172.20.10.2:8000/Soccer/SoccerClient/build/esp32.esp32.esp32/SoccerClient.ino.bin");
  Serial.println("===========================");
}

static void handleSerialServer(){
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n'); line.trim();
  if (!line.length()) return;

  // collapse whitespace
  String norm; norm.reserve(line.length()); bool sp=false;
  for (size_t i=0;i<line.length();++i){ char c=line[i]; if (c==' '||c=='\t'){ if(!sp){norm+=' '; sp=true;} } else { norm+=c; sp=false; } }

  int sp1 = norm.indexOf(' ');
  String cmd = (sp1<0)? norm : norm.substring(0,sp1); cmd.toUpperCase(); cmd.trim();

  String arg1="", rest="";
  if (sp1>=0){ rest=norm.substring(sp1+1); rest.trim(); int sp2=rest.indexOf(' ');
    if (sp2<0){ arg1=rest; rest=""; } else { arg1=rest.substring(0,sp2); arg1.trim(); rest=rest.substring(sp2+1); rest.trim(); } }

  if (cmd=="HELP"){ printHelp(); return; }
  if (cmd=="LIST"){ helloDripActive=true; helloDripRemain=HELLO_DRIP_COUNT; helloNextAt=millis(); listPrintAt=0; Serial.println("Requesting HELLOs (drip)..."); return; }
  if (cmd=="WIFI"){ if(!arg1.length()){Serial.println("Usage: WIFI <ALL|name> <ssid> <pass>");return;}
    int sp=rest.indexOf(' '); if(sp<0){Serial.println("Usage: WIFI <ALL|name> <ssid> <pass>");return;}
    String ssid=rest.substring(0,sp), pass=rest.substring(sp+1); ssid.trim(); pass.trim();
    if(!ssid.length()||!pass.length()){Serial.println("Usage: WIFI <ALL|name> <ssid> <pass>");return;}
    sendWifiSet(arg1.c_str(),ssid,pass); Serial.printf("Sent WIFI_SET to %s (ssid=%s)\n",arg1.c_str(),ssid.c_str()); return; }
  if (cmd=="OTA"){ if(!arg1.length()||!rest.length()){Serial.println("Usage: OTA <ALL|name> <url>"); return;}
    sendOta(arg1.c_str(),rest); Serial.printf("Sent OTA_TRIGGER to %s  url=%s\n",arg1.c_str(),rest.c_str()); return; }
  if (cmd=="NAME"){ if(!arg1.length()||!rest.length()){Serial.println("Usage: NAME <ALL|currentName> <newName>");return;}
    sendNameSet(arg1.c_str(),rest); Serial.printf("Sent NAME_SET to %s -> %s\n",arg1.c_str(),rest.c_str()); return; }

  Serial.println("Unknown command. Type HELP.");
}

void setup(){
  Serial.begin(115200); delay(200);
  nvsSaveName(myName);

  wifiStaOnCh6();
  if (!nowInit()) Serial.println("ESP-NOW init failed!");
  esp_now_register_recv_cb(onNowRecv);

  HelloMsg hm{}; hm.h.kind=MSG_HELLO; strncpy(hm.h.target,"ALL",sizeof(hm.h.target)-1);
  strncpy(hm.name,myName.c_str(),sizeof(hm.name)-1); esp_now_send(BCAST,(uint8_t*)&hm,sizeof(hm));

  Serial.println("Server ready. Type HELP for commands.");
}

void loop(){
  handleSerialServer();

  // HELLO drip
  uint32_t now = millis();
  if (helloDripActive){
    if (helloDripRemain && (int32_t)(now - helloNextAt) >= 0){
      sendHelloReqAll(); helloDripRemain--; helloNextAt=now+HELLO_DRIP_INTERVAL_MS;
      if (helloDripRemain==0){ helloDripActive=false; listPrintAt=helloNextAt+HELLO_SETTLE_MS; }
    }
  }
  if (listPrintAt && (int32_t)(now - listPrintAt) >= 0){
    listPrintAt=0; Serial.println("Last seen clients:");
    for (int i=0;i<32;i++) if (seen[i].name.length())
      Serial.printf(" - %s  (%lus ago)\n", seen[i].name.c_str(), (unsigned long)((now - seen[i].ms)/1000));
  }

  // Push LED overlays
  if ((int32_t)(now - nextLedPushAt) >= 0){
    pushLedStates();
    nextLedPushAt = now + LED_PUSH_MIN_MS;
  }

  delay(2);
}
