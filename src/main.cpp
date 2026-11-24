// =============================================================================
//  main.cpp  -  Nha Thong Minh tren ESP32 DevKit (Arduino framework)
//
//  Thiet bi: RFID RC522, PIR HC-SR501, DHT11, Servo SG90, Relay 3 kenh.
//  Dieu khien tu App: WebServer (HTTP REST + trang web) + WebSocket (realtime).
//
//  Toan bo loop chay non-blocking bang millis() -> khong lam treo RFID/App.
//  Cau hinh chan / Wi-Fi / nguong / UID the: xem file include/config.h
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <MFRC522.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <Preferences.h>   // Luu SSID/PASS Wi-Fi vao bo nho NVS (khong mat khi tat nguon)
#include <Update.h>        // Nap firmware OTA qua trinh duyet

#include "config.h"

// -----------------------------------------------------------------------------
// Doi tuong phan cung
// -----------------------------------------------------------------------------
MFRC522         rfid(PIN_RFID_SS, PIN_RFID_RST);
DHT             dht(PIN_DHT, DHT11);
Servo           curtainServo;
WebServer       httpServer(80);
WebSocketsServer wsServer(81);
Preferences     prefs;        // Vung "wifi" trong NVS

// -----------------------------------------------------------------------------
// Trang thai he thong
// -----------------------------------------------------------------------------
enum ChannelMode { MODE_AUTO, MODE_MANUAL };

struct SystemState {
  // Cam bien
  float   temperature = NAN;
  float   humidity    = NAN;
  bool    motion      = false;

  // Trang thai chap hanh (true = dang BAT/mo)
  bool    lightOn     = false;
  bool    fanOn       = false;
  bool    lockOpen    = false;
  int     servoAngle  = SERVO_DEFAULT_ANGLE;

  // Che do dieu khien cho Den & Quat (AUTO: theo cam bien, MANUAL: theo App)
  ChannelMode lightMode = MODE_AUTO;
  ChannelMode fanMode   = MODE_AUTO;

  // RFID
  String  lastUid;
  bool    lastAccessGranted = false;
} state;

// Moc thoi gian (millis) cho logic non-blocking
unsigned long lastMotionTime = 0;
unsigned long lockOpenedAt   = 0;
unsigned long lastDhtRead    = 0;
unsigned long lastWsPush     = 0;

// =============================================================================
//  TIEN ICH RELAY  (ho tro ca kich muc thap va muc cao)
// =============================================================================
static inline void relayWrite(uint8_t pin, bool on) {
#if RELAY_ACTIVE_LOW
  digitalWrite(pin, on ? LOW : HIGH);   // Active LOW: LOW = ON
#else
  digitalWrite(pin, on ? HIGH : LOW);   // Active HIGH: HIGH = ON
#endif
}

// =============================================================================
//  RFID
// =============================================================================
static String uidToString(const MFRC522::Uid &uid) {
  String s;
  for (byte i = 0; i < uid.size; i++) {
    if (uid.uidByte[i] < 0x10) s += '0';
    s += String(uid.uidByte[i], HEX);
    if (i + 1 < uid.size) s += ' ';
  }
  s.toUpperCase();
  return s;
}

// ---- Quan ly the RFID dong (luu trong NVS, them/xoa tu trang web) ----
// Cac the trong config.h (AUTHORIZED_UIDS) la the CO DINH (khong xoa qua web).
// Cac the them tu web duoc luu vao NVS -> khong mat khi tat nguon.
#define MAX_RFID_CARDS 30

struct RfidCard {
  String uid;     // Dang chuan "A1 B2 C3 D4" (HOA, cach nhau 1 dau cach)
  String name;    // Ten goi nho (vd "Bo", "The phong khach")
};
static RfidCard gCards[MAX_RFID_CARDS];
static int      gCardCount = 0;

// Chuan hoa UID nguoi dung nhap (bo ky tu thua, HOA, cach nhau 1 dau cach).
// Chap nhan "A1B2C3D4", "a1:b2:c3:d4", "A1 B2 C3 D4"... -> "A1 B2 C3 D4"
static String normalizeUid(const String &in) {
  String hex;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (isxdigit((int)c)) hex += (char)toupper(c);
  }
  if (hex.length() < 2 || (hex.length() % 2)) return String("");
  String out;
  for (size_t i = 0; i < hex.length(); i += 2) {
    if (i) out += ' ';
    out += hex.substring(i, i + 2);
  }
  return out;
}

// Doc danh sach the tu NVS vao RAM (goi 1 lan luc khoi dong)
static void loadCards() {
  gCardCount = 0;
  prefs.begin("rfid", true);                 // true = chi doc
  String js = prefs.getString("list", "[]");
  prefs.end();

  JsonDocument doc;
  if (deserializeJson(doc, js)) return;      // Loi parse -> coi nhu rong
  for (JsonObject o : doc.as<JsonArray>()) {
    if (gCardCount >= MAX_RFID_CARDS) break;
    String uid  = (const char *)(o["uid"]  | "");
    String name = (const char *)(o["name"] | "");
    if (uid.length()) {
      gCards[gCardCount].uid  = uid;
      gCards[gCardCount].name = name;
      gCardCount++;
    }
  }
  Serial.printf("[RFID] Da nap %d the tu NVS\n", gCardCount);
}

// Ghi danh sach the tu RAM xuong NVS
static bool saveCards() {
  JsonDocument doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < gCardCount; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["uid"]  = gCards[i].uid;
    o["name"] = gCards[i].name;
  }
  String out;
  serializeJson(doc, out);
  prefs.begin("rfid", false);                // false = ghi
  size_t n = prefs.putString("list", out);
  prefs.end();
  return n > 0;
}

static bool isAuthorized(const String &uid) {
  // 1) The co dinh trong config.h
  for (size_t i = 0; i < sizeof(AUTHORIZED_UIDS) / sizeof(AUTHORIZED_UIDS[0]); i++) {
    if (uid.equalsIgnoreCase(AUTHORIZED_UIDS[i])) return true;
  }
  // 2) The dong luu trong NVS (them tu web)
  for (int i = 0; i < gCardCount; i++) {
    if (uid.equalsIgnoreCase(gCards[i].uid)) return true;
  }
  return false;
}

// Them the moi. Tra ve: 0=OK, -1=UID sai, -2=da ton tai, -3=day, -4=loi luu
static int addCard(const String &rawUid, const String &name) {
  String uid = normalizeUid(rawUid);
  if (uid.isEmpty())        return -1;
  if (isAuthorized(uid))    return -2;
  if (gCardCount >= MAX_RFID_CARDS) return -3;
  gCards[gCardCount].uid  = uid;
  gCards[gCardCount].name = name;
  gCardCount++;
  return saveCards() ? 0 : -4;
}

// Xoa the dong (khong xoa duoc the co dinh trong config.h)
static bool removeCard(const String &rawUid) {
  String uid = normalizeUid(rawUid);
  for (int i = 0; i < gCardCount; i++) {
    if (gCards[i].uid.equalsIgnoreCase(uid)) {
      for (int j = i; j < gCardCount - 1; j++) gCards[j] = gCards[j + 1];
      gCardCount--;
      return saveCards();
    }
  }
  return false;
}

// Dong ren/mo khoa dieu khien relay kenh 3
static void openLock() {
  state.lockOpen = true;
  lockOpenedAt   = millis();
  relayWrite(PIN_RELAY_LOCK, true);
}

static void handleRfid() {
  if (!rfid.PICC_IsNewCardPresent()) return;   // Khong co the moi -> thoat ngay
  if (!rfid.PICC_ReadCardSerial())   return;   // Doc UID that bai -> thoat

  String uid = uidToString(rfid.uid);
  state.lastUid = uid;

  if (isAuthorized(uid)) {
    state.lastAccessGranted = true;
    Serial.printf("[RFID] The HOP LE: %s -> MO KHOA\n", uid.c_str());
    openLock();
  } else {
    state.lastAccessGranted = false;
    Serial.printf("[RFID] The TU CHOI: %s (giu khoa dong)\n", uid.c_str());
  }

  rfid.PICC_HaltA();        // Dung giao tiep voi the hien tai
  rfid.PCD_StopCrypto1();
}

// Tu dong dong khoa sau LOCK_OPEN_MS
static void updateLock() {
  if (state.lockOpen && (millis() - lockOpenedAt >= LOCK_OPEN_MS)) {
    state.lockOpen = false;
    relayWrite(PIN_RELAY_LOCK, false);
    Serial.println("[LOCK] Het thoi gian -> dong khoa");
  }
}

// =============================================================================
//  PIR -> DEN (relay kenh 1)  [chi khi lightMode == AUTO]
// =============================================================================
static void handlePir() {
  bool motion = digitalRead(PIN_PIR) == HIGH;
  state.motion = motion;

  if (state.lightMode != MODE_AUTO) return;   // App dang dieu khien tay -> bo qua PIR

  if (motion) {
    lastMotionTime = millis();
    if (!state.lightOn) {
      state.lightOn = true;
      relayWrite(PIN_RELAY_LIGHT, true);
      Serial.println("[PIR] Co chuyen dong -> BAT den");
    }
  } else if (state.lightOn && (millis() - lastMotionTime >= LIGHT_HOLD_MS)) {
    state.lightOn = false;
    relayWrite(PIN_RELAY_LIGHT, false);
    Serial.println("[PIR] Het chuyen dong -> TAT den");
  }
}

// =============================================================================
//  DHT11 -> QUAT (relay kenh 2)  [chi khi fanMode == AUTO]
// =============================================================================
static void handleDht() {
  if (millis() - lastDhtRead < DHT_READ_MS) return;
  lastDhtRead = millis();

  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h)) {
    Serial.println("[DHT] Loi doc cam bien!");
    return;
  }
  state.temperature = t;
  state.humidity    = h;

  if (state.fanMode != MODE_AUTO) return;     // App dieu khien tay -> bo qua tu dong

  if (!state.fanOn && t > TEMP_FAN_ON) {
    state.fanOn = true;
    relayWrite(PIN_RELAY_FAN, true);
    Serial.printf("[DHT] %.1f°C > %.1f -> BAT quat\n", t, TEMP_FAN_ON);
  } else if (state.fanOn && t <= TEMP_FAN_OFF) {
    state.fanOn = false;
    relayWrite(PIN_RELAY_FAN, false);
    Serial.printf("[DHT] %.1f°C <= %.1f -> TAT quat\n", t, TEMP_FAN_OFF);
  }
}

// =============================================================================
//  SERVO REN (GPIO 13)
// =============================================================================
static void setServoAngle(int angle) {
  angle = constrain(angle, 0, 180);
  state.servoAngle = angle;
  curtainServo.write(angle);
  Serial.printf("[SERVO] Ren -> %d°\n", angle);
}

// =============================================================================
//  PHAN XU LY LENH TU APP  (dung chung cho WebSocket va REST)
//  Nhan JSON, tra ve chuoi mo ta ket qua.
//  Vi du payload:
//    {"cmd":"servo","angle":120}
//    {"cmd":"curtain","state":"open"}      // open | close
//    {"cmd":"light","mode":"auto"}         // ve che do tu dong (PIR)
//    {"cmd":"light","state":"on"}          // bat/tat tay -> chuyen MANUAL
//    {"cmd":"fan","state":"off"}           // fan tuong tu light
//    {"cmd":"lock","action":"open"}        // mo khoa tu xa
//    {"cmd":"status"}                      // yeu cau tra trang thai
// =============================================================================
static String handleAppCommand(const String &json) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) return String("{\"ok\":false,\"err\":\"json\"}");

  const char *cmd = doc["cmd"] | "";

  if (!strcmp(cmd, "servo")) {
    if (doc["angle"].is<int>()) setServoAngle(doc["angle"].as<int>());

  } else if (!strcmp(cmd, "curtain")) {
    const char *st = doc["state"] | "";
    if (!strcmp(st, "open"))  setServoAngle(SERVO_CURTAIN_OPEN);
    if (!strcmp(st, "close")) setServoAngle(SERVO_CURTAIN_CLOSED);

  } else if (!strcmp(cmd, "light")) {
    const char *mode = doc["mode"] | "";
    const char *st   = doc["state"] | "";
    if (!strcmp(mode, "auto")) {
      state.lightMode = MODE_AUTO;                 // Tra quyen cho PIR
    } else if (*st) {
      state.lightMode = MODE_MANUAL;
      state.lightOn   = !strcmp(st, "on");
      relayWrite(PIN_RELAY_LIGHT, state.lightOn);
    }

  } else if (!strcmp(cmd, "fan")) {
    const char *mode = doc["mode"] | "";
    const char *st   = doc["state"] | "";
    if (!strcmp(mode, "auto")) {
      state.fanMode = MODE_AUTO;                    // Tra quyen cho DHT
    } else if (*st) {
      state.fanMode = MODE_MANUAL;
      state.fanOn   = !strcmp(st, "on");
      relayWrite(PIN_RELAY_FAN, state.fanOn);
    }

  } else if (!strcmp(cmd, "lock")) {
    const char *action = doc["action"] | "";
    if (!strcmp(action, "open")) openLock();        // Mo khoa tu xa (van tu dong dong lai)

  } else if (strcmp(cmd, "status")) {
    return String("{\"ok\":false,\"err\":\"unknown_cmd\"}");
  }

  return String("{\"ok\":true}");
}

// =============================================================================
//  SERIALIZE TRANG THAI -> JSON (gui len App)
// =============================================================================
static String buildStatusJson() {
  JsonDocument doc;
  doc["type"]        = "status";
  if (isnan(state.temperature)) doc["temperature"] = nullptr; else doc["temperature"] = state.temperature;
  if (isnan(state.humidity))    doc["humidity"]    = nullptr; else doc["humidity"]    = state.humidity;
  doc["motion"]      = state.motion;
  doc["light"]       = state.lightOn;
  doc["fan"]         = state.fanOn;
  doc["lock"]        = state.lockOpen;
  doc["servo"]       = state.servoAngle;
  doc["lightMode"]   = state.lightMode == MODE_AUTO ? "auto" : "manual";
  doc["fanMode"]     = state.fanMode   == MODE_AUTO ? "auto" : "manual";
  doc["lastUid"]     = state.lastUid;
  doc["access"]      = state.lastAccessGranted;

  String out;
  serializeJson(doc, out);
  return out;
}

static void pushStatus() {
  String msg = buildStatusJson();
  wsServer.broadcastTXT(msg);
}

// =============================================================================
//  WEBSOCKET
// =============================================================================
static void onWsEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.printf("[WS] Client #%u da ket noi\n", num);
      {
        String st = buildStatusJson();
        wsServer.sendTXT(num, st);                // Gui trang thai ngay khi ket noi
      }
      break;
    case WStype_TEXT: {
      String msg = String((char *)payload, length);
      String res = handleAppCommand(msg);
      wsServer.sendTXT(num, res);
      pushStatus();                               // Phan hoi trang thai moi cho tat ca
      break;
    }
    default:
      break;
  }
}

// =============================================================================
//  WEBSERVER (REST + trang dieu khien)
// =============================================================================
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="vi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Nha Thong Minh</title>
<style>
 body{font-family:system-ui,Arial;margin:0;background:#0f172a;color:#e2e8f0}
 .wrap{max-width:560px;margin:0 auto;padding:16px}
 h1{font-size:20px}
 .card{background:#1e293b;border-radius:12px;padding:16px;margin:12px 0}
 .row{display:flex;justify-content:space-between;align-items:center;margin:8px 0}
 .pill{padding:2px 10px;border-radius:999px;font-size:13px}
 .on{background:#16a34a}.off{background:#475569}
 button{background:#2563eb;color:#fff;border:0;border-radius:8px;padding:8px 14px;margin:2px;cursor:pointer}
 button.sec{background:#475569}
 input[type=range]{width:100%}
 .muted{color:#94a3b8;font-size:13px}
</style></head><body><div class="wrap">
<h1>🏠 Nha Thong Minh — ESP32</h1>
<div class="card">
  <div class="row"><span>Nhiet do</span><b id="temp">--</b></div>
  <div class="row"><span>Do am</span><b id="humi">--</b></div>
  <div class="row"><span>Chuyen dong (PIR)</span><span id="motion" class="pill off">--</span></div>
  <div class="row"><span>Den</span><span id="light" class="pill off">--</span></div>
  <div class="row"><span>Quat</span><span id="fan" class="pill off">--</span></div>
  <div class="row"><span>Khoa</span><span id="lock" class="pill off">--</span></div>
  <div class="row muted"><span>The vua quet</span><span id="uid">--</span></div>
</div>
<div class="card">
  <h3>Ren cua (Servo)</h3>
  <input type="range" min="0" max="180" value="0" id="servo"
         oninput="document.getElementById('sv').innerText=this.value"
         onchange="send({cmd:'servo',angle:+this.value})">
  <div class="row"><span class="muted">Goc: <b id="sv">0</b>°</span>
    <span><button onclick="send({cmd:'curtain',state:'open'})">Mo ren</button>
    <button class="sec" onclick="send({cmd:'curtain',state:'close'})">Dong ren</button></span></div>
</div>
<div class="card">
  <h3>Dieu khien tay</h3>
  <div class="row"><span>Den</span><span>
    <button onclick="send({cmd:'light',state:'on'})">Bat</button>
    <button class="sec" onclick="send({cmd:'light',state:'off'})">Tat</button>
    <button class="sec" onclick="send({cmd:'light',mode:'auto'})">Auto (PIR)</button></span></div>
  <div class="row"><span>Quat</span><span>
    <button onclick="send({cmd:'fan',state:'on'})">Bat</button>
    <button class="sec" onclick="send({cmd:'fan',state:'off'})">Tat</button>
    <button class="sec" onclick="send({cmd:'fan',mode:'auto'})">Auto (DHT)</button></span></div>
  <div class="row"><span>Khoa</span>
    <button onclick="send({cmd:'lock',action:'open'})">Mo khoa</button></div>
</div>
<div class="row muted" style="justify-content:center;gap:16px">
  <a href="/rfid" style="color:#60a5fa">🔑 Quan ly the RFID</a>
  <a href="/settings" style="color:#60a5fa">⚙️ Cai dat Wi-Fi &amp; Nap OTA</a>
</div>
<script>
 let ws;
 function connect(){
   ws=new WebSocket('ws://'+location.hostname+':81/');
   ws.onmessage=e=>{try{update(JSON.parse(e.data))}catch(_){}};
   ws.onclose=()=>setTimeout(connect,1500);
 }
 function send(o){ if(ws&&ws.readyState==1) ws.send(JSON.stringify(o)); }
 function pill(id,on,txt){let el=document.getElementById(id);el.className='pill '+(on?'on':'off');el.innerText=txt;}
 function update(s){
   if(s.type!=='status')return;
   document.getElementById('temp').innerText=(s.temperature==null?'--':s.temperature.toFixed(1)+' °C');
   document.getElementById('humi').innerText=(s.humidity==null?'--':s.humidity.toFixed(0)+' %');
   pill('motion',s.motion,s.motion?'Co':'Khong');
   pill('light',s.light,(s.light?'BAT':'TAT')+' ('+s.lightMode+')');
   pill('fan',s.fan,(s.fan?'BAT':'TAT')+' ('+s.fanMode+')');
   pill('lock',s.lock,s.lock?'MO':'DONG');
   document.getElementById('uid').innerText=s.lastUid?(s.lastUid+(s.access?' ✓':' ✗')):'--';
   let sv=document.getElementById('servo'); sv.value=s.servo; document.getElementById('sv').innerText=s.servo;
 }
 connect();
</script>
</div></body></html>
)HTML";

// =============================================================================
//  TRANG CAI DAT: Wi-Fi + OTA
// =============================================================================
static const char SETTINGS_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="vi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Cai dat - Nha Thong Minh</title>
<style>
 body{font-family:system-ui,Arial;margin:0;background:#0f172a;color:#e2e8f0}
 .wrap{max-width:560px;margin:0 auto;padding:16px}
 h1{font-size:20px}h3{margin:0 0 10px}
 .card{background:#1e293b;border-radius:12px;padding:16px;margin:12px 0}
 label{display:block;font-size:13px;color:#94a3b8;margin:10px 0 4px}
 input{width:100%;box-sizing:border-box;padding:9px;border-radius:8px;border:1px solid #334155;background:#0f172a;color:#e2e8f0}
 button{background:#2563eb;color:#fff;border:0;border-radius:8px;padding:10px 16px;margin-top:12px;cursor:pointer;width:100%}
 .muted{color:#94a3b8;font-size:13px}
 .bar{height:10px;background:#334155;border-radius:999px;overflow:hidden;margin-top:10px;display:none}
 .bar>div{height:100%;width:0;background:#16a34a;transition:width .2s}
 #msg{margin-top:10px;font-size:14px}
 a{color:#60a5fa}
</style></head><body><div class="wrap">
<h1>⚙️ Cai dat</h1>

<div class="card">
  <h3>📶 Wi-Fi</h3>
  <div class="muted">Dang ket noi: <b id="cur">...</b></div>
  <form id="wf">
    <label>Ten Wi-Fi (SSID)</label>
    <input name="ssid" id="ssid" placeholder="Ten mang Wi-Fi" required>
    <label>Mat khau</label>
    <input name="pass" id="pass" type="password" placeholder="Mat khau Wi-Fi">
    <button type="submit">Luu &amp; Khoi dong lai</button>
  </form>
  <div class="muted" style="margin-top:8px">Sau khi luu, ESP32 khoi dong lai va ket noi mang moi.
  Neu sai mat khau, thiet bi se tu phat Wi-Fi rieng de bạn vao lai.</div>
</div>

<div class="card">
  <h3>⬆️ Nap firmware OTA</h3>
  <div class="muted">Chon file <b>firmware.bin</b> (thuong o .pio/build/.../firmware.bin).</div>
  <form id="of">
    <input type="file" name="update" id="fw" accept=".bin" required>
    <button type="submit">Nap firmware</button>
  </form>
  <div class="bar" id="bar"><div id="prog"></div></div>
  <div id="msg"></div>
</div>

<div class="card"><a href="/">&larr; Ve trang dieu khien</a></div>

<script>
 fetch('/wifiinfo').then(r=>r.json()).then(j=>{
   document.getElementById('cur').innerText=j.connected?(j.ssid+' ('+j.ip+')'):('Che do AP - '+j.ip);
   document.getElementById('ssid').value=j.ssid||'';
 }).catch(()=>{});

 document.getElementById('wf').onsubmit=e=>{
   e.preventDefault();
   let f=new FormData(e.target);
   fetch('/wifi',{method:'POST',body:new URLSearchParams(f)})
     .then(r=>r.text()).then(t=>{alert(t);});
 };

 document.getElementById('of').onsubmit=e=>{
   e.preventDefault();
   let file=document.getElementById('fw').files[0];
   if(!file)return;
   let xhr=new XMLHttpRequest(), bar=document.getElementById('bar'), prog=document.getElementById('prog'), msg=document.getElementById('msg');
   bar.style.display='block'; msg.innerText='Dang nap...';
   xhr.upload.onprogress=ev=>{ if(ev.lengthComputable){let p=Math.round(ev.loaded/ev.total*100); prog.style.width=p+'%'; msg.innerText='Dang nap... '+p+'%';}};
   xhr.onloadend=()=>{ msg.innerText=(xhr.status==200?'✅ Thanh cong! ESP32 dang khoi dong lai...':'❌ Loi: '+xhr.responseText);};
   xhr.open('POST','/ota');
   let fd=new FormData(); fd.append('update',file);
   xhr.send(fd);
 };
</script>
</div></body></html>
)HTML";

// =============================================================================
//  TRANG QUAN LY THE RFID
// =============================================================================
static const char RFID_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="vi"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Quan ly the RFID</title>
<style>
 body{font-family:system-ui,Arial;margin:0;background:#0f172a;color:#e2e8f0}
 .wrap{max-width:560px;margin:0 auto;padding:16px}
 h1{font-size:20px}h3{margin:0 0 10px}
 .card{background:#1e293b;border-radius:12px;padding:16px;margin:12px 0}
 label{display:block;font-size:13px;color:#94a3b8;margin:10px 0 4px}
 input{width:100%;box-sizing:border-box;padding:9px;border-radius:8px;border:1px solid #334155;background:#0f172a;color:#e2e8f0}
 button{background:#2563eb;color:#fff;border:0;border-radius:8px;padding:9px 14px;margin-top:10px;cursor:pointer}
 button.sec{background:#475569}
 button.del{background:#dc2626;padding:6px 10px;margin:0}
 .muted{color:#94a3b8;font-size:13px}
 .scan{font-size:18px;font-weight:700;letter-spacing:1px}
 table{width:100%;border-collapse:collapse;margin-top:8px}
 td,th{text-align:left;padding:8px 6px;border-bottom:1px solid #334155;font-size:14px}
 .tag{font-size:11px;padding:2px 8px;border-radius:999px;background:#475569;color:#cbd5e1}
 a{color:#60a5fa}
</style></head><body><div class="wrap">
<h1>🔑 Quan ly the RFID</h1>

<div class="card">
  <h3>1) Quet the de lay UID</h3>
  <div class="muted">Dua the vao dau doc RC522, UID se hien ngay ben duoi.</div>
  <div class="row" style="margin-top:8px">The vua quet: <span class="scan" id="scan">--</span></div>
  <button class="sec" onclick="useScan()">⬇️ Dung UID nay de them</button>
</div>

<div class="card">
  <h3>2) Them the</h3>
  <form id="af">
    <label>UID the</label>
    <input id="uid" name="uid" placeholder="VD: A1 B2 C3 D4" required>
    <label>Ten goi nho</label>
    <input id="name" name="name" placeholder="VD: The cua Bo">
    <button type="submit">➕ Them the</button>
  </form>
  <div id="msg" class="muted" style="margin-top:8px"></div>
</div>

<div class="card">
  <h3>3) Danh sach the (<span id="cnt">0</span>)</h3>
  <table><thead><tr><th>Ten</th><th>UID</th><th></th></tr></thead>
  <tbody id="tb"></tbody></table>
</div>

<div class="card"><a href="/">&larr; Ve trang dieu khien</a></div>

<script>
 let ws, lastScan='';
 function connect(){
   ws=new WebSocket('ws://'+location.hostname+':81/');
   ws.onmessage=e=>{try{let s=JSON.parse(e.data); if(s.type==='status'&&s.lastUid){lastScan=s.lastUid; document.getElementById('scan').innerText=s.lastUid+(s.access?' ✓ (da co)':' ✗ (chua co)');}}catch(_){}};
   ws.onclose=()=>setTimeout(connect,1500);
 }
 function useScan(){ if(lastScan) document.getElementById('uid').value=lastScan; }
 function load(){
   fetch('/rfid/list').then(r=>r.json()).then(j=>{
     document.getElementById('cnt').innerText=j.length;
     let tb=document.getElementById('tb'); tb.innerHTML='';
     j.forEach(c=>{
       let tr=document.createElement('tr');
       let act = c.fixed ? '<span class="tag">co dinh</span>'
                         : '<button class="del" data-uid="'+c.uid+'">Xoa</button>';
       tr.innerHTML='<td>'+(c.name||'-')+'</td><td class="muted">'+c.uid+'</td><td>'+act+'</td>';
       tb.appendChild(tr);
     });
     tb.querySelectorAll('button.del').forEach(b=>b.onclick=()=>del(b.dataset.uid));
   });
 }
 function del(uid){
   if(!confirm('Xoa the '+uid+' ?'))return;
   fetch('/rfid/del',{method:'POST',body:new URLSearchParams({uid})})
     .then(r=>r.text()).then(t=>{document.getElementById('msg').innerText=t; load();});
 }
 document.getElementById('af').onsubmit=e=>{
   e.preventDefault();
   let f=new URLSearchParams(new FormData(e.target));
   fetch('/rfid/add',{method:'POST',body:f})
     .then(r=>r.text()).then(t=>{document.getElementById('msg').innerText=t; document.getElementById('uid').value=''; document.getElementById('name').value=''; load();});
 };
 connect(); load();
</script>
</div></body></html>
)HTML";

// Chuyen tham so REST (vd /servo?angle=90) thanh JSON roi dua vao handleAppCommand
static void restCommand(const String &json) {
  String res = handleAppCommand(json);
  httpServer.send(200, "application/json", res);
  pushStatus();
}

static void setupHttpRoutes() {
  httpServer.on("/", HTTP_GET, []() {
    httpServer.send_P(200, "text/html", INDEX_HTML);
  });
  httpServer.on("/status", HTTP_GET, []() {
    httpServer.send(200, "application/json", buildStatusJson());
  });
  httpServer.on("/servo", HTTP_GET, []() {
    restCommand("{\"cmd\":\"servo\",\"angle\":" + httpServer.arg("angle") + "}");
  });
  httpServer.on("/curtain", HTTP_GET, []() {
    restCommand("{\"cmd\":\"curtain\",\"state\":\"" + httpServer.arg("state") + "\"}");
  });
  httpServer.on("/light", HTTP_GET, []() {
    String mode = httpServer.arg("mode");
    if (mode.length()) restCommand("{\"cmd\":\"light\",\"mode\":\"" + mode + "\"}");
    else restCommand("{\"cmd\":\"light\",\"state\":\"" + httpServer.arg("state") + "\"}");
  });
  httpServer.on("/fan", HTTP_GET, []() {
    String mode = httpServer.arg("mode");
    if (mode.length()) restCommand("{\"cmd\":\"fan\",\"mode\":\"" + mode + "\"}");
    else restCommand("{\"cmd\":\"fan\",\"state\":\"" + httpServer.arg("state") + "\"}");
  });
  httpServer.on("/lock", HTTP_GET, []() {
    restCommand("{\"cmd\":\"lock\",\"action\":\"open\"}");
  });

  // ---- Trang cai dat ----
  httpServer.on("/settings", HTTP_GET, []() {
    httpServer.send_P(200, "text/html", SETTINGS_HTML);
  });

  // Tra ve SSID dang dung + IP + trang thai ket noi (cho trang settings)
  httpServer.on("/wifiinfo", HTTP_GET, []() {
    prefs.begin("wifi", true);
    String ssid = prefs.getString("ssid", WIFI_SSID);
    prefs.end();
    bool connected = (WiFi.status() == WL_CONNECTED);
    String ip = connected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    JsonDocument doc;
    doc["ssid"] = ssid;
    doc["ip"] = ip;
    doc["connected"] = connected;
    String out; serializeJson(doc, out);
    httpServer.send(200, "application/json", out);
  });

  // Luu Wi-Fi moi vao NVS roi khoi dong lai
  httpServer.on("/wifi", HTTP_POST, []() {
    String ssid = httpServer.arg("ssid");
    String pass = httpServer.arg("pass");
    if (ssid.isEmpty()) {
      httpServer.send(400, "text/plain", "Thieu ten Wi-Fi (SSID).");
      return;
    }
    prefs.begin("wifi", false);                    // false = ghi
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    prefs.end();
    Serial.printf("[WiFi] Da luu SSID moi: \"%s\" -> khoi dong lai\n", ssid.c_str());
    httpServer.send(200, "text/plain", "Da luu. ESP32 dang khoi dong lai de ket noi mang moi...");
    delay(400);
    ESP.restart();
  });

  // ---- Nap firmware OTA ----
  httpServer.on("/ota", HTTP_POST,
    []() {   // Goi SAU khi nhan xong file
      bool ok = !Update.hasError();
      httpServer.send(200, "text/plain", ok ? "OK" : "FAIL");
      delay(400);
      ESP.restart();
    },
    []() {   // Nhan file theo tung khoi (streaming)
      HTTPUpload &up = httpServer.upload();
      if (up.status == UPLOAD_FILE_START) {
        Serial.printf("[OTA] Bat dau nap: %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
      } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
      } else if (up.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("[OTA] Xong %u bytes. Khoi dong lai...\n", up.totalSize);
        else Update.printError(Serial);
      }
    }
  );

  // ---- Quan ly the RFID ----
  httpServer.on("/rfid", HTTP_GET, []() {
    httpServer.send_P(200, "text/html", RFID_HTML);
  });

  // Danh sach the (ca the co dinh trong config.h lan the dong trong NVS)
  httpServer.on("/rfid/list", HTTP_GET, []() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (size_t i = 0; i < sizeof(AUTHORIZED_UIDS) / sizeof(AUTHORIZED_UIDS[0]); i++) {
      JsonObject o = arr.add<JsonObject>();
      o["uid"]   = AUTHORIZED_UIDS[i];
      o["name"]  = "(config.h)";
      o["fixed"] = true;
    }
    for (int i = 0; i < gCardCount; i++) {
      JsonObject o = arr.add<JsonObject>();
      o["uid"]   = gCards[i].uid;
      o["name"]  = gCards[i].name;
      o["fixed"] = false;
    }
    String out; serializeJson(doc, out);
    httpServer.send(200, "application/json", out);
  });

  // Them the moi
  httpServer.on("/rfid/add", HTTP_POST, []() {
    int r = addCard(httpServer.arg("uid"), httpServer.arg("name"));
    const char *m;
    switch (r) {
      case 0:  m = "✅ Da them the."; break;
      case -1: m = "❌ UID khong hop le."; break;
      case -2: m = "⚠️ The nay da ton tai."; break;
      case -3: m = "❌ Da day danh sach the."; break;
      default: m = "❌ Loi luu vao bo nho."; break;
    }
    httpServer.send(r == 0 ? 200 : 400, "text/plain; charset=utf-8", m);
  });

  // Xoa the dong
  httpServer.on("/rfid/del", HTTP_POST, []() {
    bool ok = removeCard(httpServer.arg("uid"));
    httpServer.send(ok ? 200 : 400, "text/plain; charset=utf-8",
                    ok ? "🗑️ Da xoa the." : "❌ Khong tim thay the (hoac the co dinh).");
  });

  httpServer.onNotFound([]() { httpServer.send(404, "text/plain", "Not found"); });
  httpServer.begin();
}

// =============================================================================
//  WIFI
// =============================================================================
static void setupWifi() {
  // Uu tien SSID/PASS da luu trong NVS (nguoi dung cai qua trang /settings).
  // Chua co -> dung gia tri mac dinh trong config.h.
  prefs.begin("wifi", true);                       // true = chi doc
  String ssid = prefs.getString("ssid", WIFI_SSID);
  String pass = prefs.getString("pass", WIFI_PASSWORD);
  prefs.end();

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(WIFI_HOSTNAME);
  WiFi.begin(ssid.c_str(), pass.c_str());
  Serial.printf("[WiFi] Dang ket noi toi \"%s\" ...\n", ssid.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[WiFi] OK! Mo trinh duyet: http://%s/\n", WiFi.localIP().toString().c_str());
    return;
  }

  // Khong ket noi duoc Wi-Fi nha -> chuyen sang CHE DO PHAT (Access Point)
  Serial.println("[WiFi] KHONG ket noi duoc -> bat che do PHAT Wi-Fi (AP).");
  WiFi.mode(WIFI_AP);
  const char *apPass = (strlen(AP_PASSWORD) >= 8) ? AP_PASSWORD : nullptr;  // < 8 ky tu -> AP mo
  bool ok = WiFi.softAP(AP_SSID, apPass);
  if (ok) {
    Serial.printf("[WiFi] AP dang phat: SSID=\"%s\"  Pass=\"%s\"\n",
                  AP_SSID, apPass ? AP_PASSWORD : "(mo, khong mat khau)");
    Serial.printf("[WiFi] Ket noi vao mang nay roi mo: http://%s/\n",
                  WiFi.softAPIP().toString().c_str());
  } else {
    Serial.println("[WiFi] Khong bat duoc AP -> chi chay cac chuc nang tu dong (RFID/PIR/DHT).");
  }
}

// =============================================================================
//  SETUP
// =============================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== KHOI DONG NHA THONG MINH (ESP32) ===");

  // Relay: dat OUTPUT va dua ve TAT (khoa DONG) truoc tien
  pinMode(PIN_RELAY_LIGHT, OUTPUT);
  pinMode(PIN_RELAY_FAN,   OUTPUT);
  pinMode(PIN_RELAY_LOCK,  OUTPUT);
  relayWrite(PIN_RELAY_LIGHT, false);
  relayWrite(PIN_RELAY_FAN,   false);
  relayWrite(PIN_RELAY_LOCK,  false);

  // PIR
  pinMode(PIN_PIR, INPUT);

  // SPI + RFID
  SPI.begin(PIN_RFID_SCK, PIN_RFID_MISO, PIN_RFID_MOSI, PIN_RFID_SS);
  rfid.PCD_Init();
  Serial.print("[RFID] RC522 version: ");
  Serial.println(rfid.PCD_ReadRegister(MFRC522::VersionReg), HEX);
  loadCards();                                   // Nap danh sach the da luu trong NVS

  // DHT11
  dht.begin();

  // Servo (ESP32Servo): cap phat timer PWM va gan chan
  // Cap ca 4 timer de thu vien tu chon kenh con trong (tranh loi "channel failed")
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  curtainServo.setPeriodHertz(50);               // SG90 = 50Hz
  curtainServo.attach(PIN_SERVO, 500, 2400);     // Do rong xung min/max (us)
  state.servoAngle = SERVO_DEFAULT_ANGLE;        // Khong write ngay luc boot -> tranh dong dinh gay brownout

  // Wi-Fi + App
  setupWifi();
  setupHttpRoutes();
  wsServer.begin();
  wsServer.onEvent(onWsEvent);

  lastMotionTime = millis();
  Serial.println("=== San sang ===");
}

// =============================================================================
//  LOOP  (non-blocking)
// =============================================================================
void loop() {
  // a) RFID -> mo khoa (kenh 3)
  handleRfid();
  updateLock();

  // b) PIR -> den (kenh 1)
  handlePir();

  // c) DHT11 -> quat (kenh 2)
  handleDht();

  // d) Servo ren + dieu khien App
  wsServer.loop();
  httpServer.handleClient();

  // Day trang thai dinh ky len App
  if (millis() - lastWsPush >= WS_PUSH_MS) {
    lastWsPush = millis();
    pushStatus();
  }
}
