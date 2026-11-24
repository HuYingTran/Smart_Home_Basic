#pragma once
// =============================================================================
//  config.h  -  Cau hinh tap trung cho mo hinh Nha Thong Minh (ESP32 DevKit)
//  Chinh sua cac gia tri o day ma khong can dong vao main.cpp.
// =============================================================================

// ----------------------------------------------------------------------------
// 1) Wi-Fi  (BAT BUOC sua lai SSID/PASS cua ban)
// ----------------------------------------------------------------------------
#define WIFI_SSID       "TEN_WIFI_CUA_BAN"
#define WIFI_PASSWORD   "MAT_KHAU_WIFI"
#define WIFI_HOSTNAME   "smart-home-esp32"
#define WIFI_TIMEOUT_MS 15000     // Thoi gian cho ket noi Wi-Fi truoc khi bo qua

// Che do phat Wi-Fi du phong (Access Point): neu KHONG ket noi duoc Wi-Fi nha,
// ESP32 tu phat 1 mang Wi-Fi rieng de dien thoai ket noi truc tiep.
// - Mat khau PHAI >= 8 ky tu. De "" neu muon AP mo (khong mat khau).
// - Sau khi ket noi vao AP nay, mo trinh duyet: http://192.168.4.1/
#define AP_SSID         "SmartHome-ESP32"
#define AP_PASSWORD     "12345678"

// ----------------------------------------------------------------------------
// 2) So do chan (PINOUT) - theo README
// ----------------------------------------------------------------------------
// RFID RC522 (SPI) - VCC = 3V3 (KHONG cap 5V)
#define PIN_RFID_SS     5
#define PIN_RFID_SCK    18
#define PIN_RFID_MOSI   23
#define PIN_RFID_MISO   19
#define PIN_RFID_RST    22

// PIR HC-SR501
#define PIN_PIR         27        // Digital input

// DHT11
#define PIN_DHT         4

// Servo SG90 (ren) - cap nguon 5V ngoai, chung GND
#define PIN_SERVO       13

// Relay 3 kenh
#define PIN_RELAY_LIGHT 26        // IN1 - Den
#define PIN_RELAY_FAN   25        // IN2 - Quat
#define PIN_RELAY_LOCK  33        // IN3 - Khoa

// ----------------------------------------------------------------------------
// 3) Muc kich Relay
//    Nhieu module relay kich muc THAP (LOW = ON, HIGH = OFF).
//    - Neu relay cua ban kich muc thap  -> de = true
//    - Neu relay kich muc cao (HIGH=ON)  -> doi thanh false
// ----------------------------------------------------------------------------
#define RELAY_ACTIVE_LOW false

// ----------------------------------------------------------------------------
// 4) Tham so hoat dong
// ----------------------------------------------------------------------------
#define LOCK_OPEN_MS    1000UL    // Giu khoa mo 1s roi CAT relay ngay (tranh cuon solenoid nong/chay)
#define LIGHT_HOLD_MS   15000UL   // Giu den sang sau khi het chuyen dong (10-30s)
#define DHT_READ_MS     2000UL    // Chu ky doc DHT11 (>= 2000ms)
#define WS_PUSH_MS      1000UL    // Chu ky day trang thai len App qua WebSocket

#define TEMP_FAN_ON     30.0f     // Nguong BAT quat
#define TEMP_FAN_OFF    28.5f     // Nguong TAT quat (hysteresis chong rung relay)

// Goc Servo cho ren
#define SERVO_CURTAIN_CLOSED 0    // Goc khi dong ren
#define SERVO_CURTAIN_OPEN   180  // Goc khi mo ren hoan toan
#define SERVO_DEFAULT_ANGLE  0    // Goc mac dinh luc khoi dong

// ----------------------------------------------------------------------------
// 5) Danh sach UID the RFID hop le
//    Quet the va xem Serial Monitor de lay UID (vd "A1 B2 C3 D4"),
//    sau do dan vao mang ben duoi.
// ----------------------------------------------------------------------------
static const char *AUTHORIZED_UIDS[] = {
    "DE AD BE EF",   // <-- THAY bang UID the cua ban
    "12 34 56 78",   // <-- (tuy chon) the thu 2
};
