/**
 * ESP32_ECG_Guardian.ino
 * ═══════════════════════════════════════════════════════════════════
 * Hardware: ESP32 + AD8232 ECG Sensor
 * 
 * Features:
 * - 125 Hz stable sampling via hardware timer interrupt
 * - Leads-off detection (LO+ / LO-)
 * - WebSocket JSON telemetry to backend
 * - Auto-reconnect for WiFi and WebSocket
 * ═══════════════════════════════════════════════════════════════════
 */

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

// ── WiFi Credentials ────────────────────────────────────────────────
const char* WIFI_SSID     = "OPPO A55";
const char* WIFI_PASSWORD = "Kish@2476";

// ── Backend WebSocket Endpoint ──────────────────────────────────────
const char* WS_HOST = "10.107.233.122";  // Replace with your backend IP
const int   WS_PORT = 8000;
const char* WS_PATH = "/ws/esp32";       // New endpoint for hardware

// ── Hardware Pin Definitions ────────────────────────────────────────
#define ECG_PIN     36   // VP/GPIO36 — ADC input from AD8232 OUTPUT
#define LO_PLUS_PIN  2   // GPIO2 — AD8232 LO+ (leads-off positive)
#define LO_MINUS_PIN 3   // GPIO3 — AD8232 LO- (leads-off negative)

// ── Sampling Configuration ──────────────────────────────────────────
#define SAMPLING_RATE_HZ  125
#define TIMER_DIVIDER     80        // 80 MHz / 80 = 1 MHz timer clock
#define TIMER_INTERVAL_US (1000000 / SAMPLING_RATE_HZ)  // 8000 µs = 125 Hz

// ── Device Identification ───────────────────────────────────────────
const char* DEVICE_ID = "ESP32_DEV_1";

// ── Global State ────────────────────────────────────────────────────
WebSocketsClient webSocket;
hw_timer_t* samplingTimer = NULL;
volatile bool sampleReady = false;
volatile int  latestADC   = 0;
volatile bool leadsOff    = false;

unsigned long lastReconnectAttempt = 0;
const unsigned long RECONNECT_INTERVAL = 5000;  // 5 seconds

// ═══════════════════════════════════════════════════════════════════
// Hardware Timer ISR (125 Hz)
// ═══════════════════════════════════════════════════════════════════
void IRAM_ATTR onTimerISR() {
  // Read ADC pin (12-bit: 0–4095)
  latestADC = analogRead(ECG_PIN);
  
  // Check leads-off status (HIGH = disconnected)
  bool loPlus  = digitalRead(LO_PLUS_PIN);
  bool loMinus = digitalRead(LO_MINUS_PIN);
  leadsOff = (loPlus || loMinus);
  
  sampleReady = true;
}

// ═══════════════════════════════════════════════════════════════════
// WebSocket Event Handler
// ═══════════════════════════════════════════════════════════════════
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.println("[WS] Disconnected");
      break;
      
    case WStype_CONNECTED:
      Serial.printf("[WS] Connected to %s:%d%s\n", WS_HOST, WS_PORT, WS_PATH);
      break;
      
    case WStype_TEXT:
      Serial.printf("[WS] Received: %s\n", payload);
      break;
      
    case WStype_ERROR:
      Serial.println("[WS] Error");
      break;
      
    default:
      break;
  }
}

// ═══════════════════════════════════════════════════════════════════
// Setup
// ═══════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println("\n\n═══════════════════════════════════════");
  Serial.println("  ESP32 ECG Guardian — Initializing");
  Serial.println("═══════════════════════════════════════");
  
  // ── Configure ADC ─────────────────────────────────────────────────
  analogReadResolution(12);  // 0–4095
  analogSetAttenuation(ADC_11db);  // 0–3.3V range
  
  // ── Configure Leads-Off Pins ──────────────────────────────────────
  pinMode(LO_PLUS_PIN,  INPUT);
  pinMode(LO_MINUS_PIN, INPUT);
  
  // ── Connect to WiFi ───────────────────────────────────────────────
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  Serial.println("\n[WiFi] Connected");
  Serial.print("[WiFi] IP Address: ");
  Serial.println(WiFi.localIP());
  
  // ── Initialize WebSocket ──────────────────────────────────────────
  webSocket.begin(WS_HOST, WS_PORT, WS_PATH);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(RECONNECT_INTERVAL);
  
  Serial.printf("[WS] Connecting to ws://%s:%d%s\n", WS_HOST, WS_PORT, WS_PATH);
  
  // ── Start Hardware Timer (125 Hz) ─────────────────────────────────
  samplingTimer = timerBegin(0, TIMER_DIVIDER, true);
  timerAttachInterrupt(samplingTimer, &onTimerISR, true);
  timerAlarmWrite(samplingTimer, TIMER_INTERVAL_US, true);
  timerAlarmEnable(samplingTimer);
  
  Serial.printf("[Timer] Sampling at %d Hz (%d µs interval)\n", SAMPLING_RATE_HZ, TIMER_INTERVAL_US);
  Serial.println("═══════════════════════════════════════");
  Serial.println("  Setup Complete — Streaming Started");
  Serial.println("═══════════════════════════════════════\n");
}

// ═══════════════════════════════════════════════════════════════════
// Main Loop
// ═══════════════════════════════════════════════════════════════════
void loop() {
  // ── WiFi Auto-Reconnect ───────────────────────────────────────────
  if (WiFi.status() != WL_CONNECTED) {
    unsigned long now = millis();
    if (now - lastReconnectAttempt > RECONNECT_INTERVAL) {
      lastReconnectAttempt = now;
      Serial.println("[WiFi] Connection lost — reconnecting...");
      WiFi.reconnect();
    }
  }
  
  // ── WebSocket Loop ────────────────────────────────────────────────
  webSocket.loop();
  
  // ── Send Sample When Ready ────────────────────────────────────────
  if (sampleReady) {
    sampleReady = false;
    
    // Convert ADC (0–4095) to voltage (0–3.3V)
    float voltage = (latestADC / 4095.0) * 3.3;
    
    // Build JSON packet
    StaticJsonDocument<128> doc;
    doc["device_id"]  = DEVICE_ID;
    doc["voltage"]    = voltage;
    doc["leads_off"]  = leadsOff;
    
    String payload;
    serializeJson(doc, payload);
    
    // Send via WebSocket
    if (webSocket.isConnected()) {
      webSocket.sendTXT(payload);
      
      // Debug print every 2 seconds (~250 samples)
      static int sampleCount = 0;
      if (++sampleCount >= 250) {
        sampleCount = 0;
        Serial.printf("[Sample] V=%.3f | Leads=%s\n", 
                      voltage, leadsOff ? "OFF" : "OK");
      }
    }
  }
  
  // Small yield to avoid watchdog reset
  yield();


    String data = String(ecgValue);


    webSocket.sendTXT(data);


    Serial.println(ecgValue);

  }

}
}
