#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiUDP.h>

// ── Configuration ─────────────────────────────────────────────────────────────
#define WIFI_SSID       "toytrains"
#define WIFI_PASS       "toytrain"
#define SPRING_HOST     "10.42.0.1"
#define SPRING_PORT     80
#define CAB_NAME        "WiFiLoco1"

#define UDP_PORT        4210
#define HEARTBEAT_MS    30000

// ── Motor pins — TB67H450FNG IN1/IN2, direct 3.3V GPIO (no level shifters) ────
// wroom32 env overrides these via build_flags (-DPIN_IN1=13 -DPIN_IN2=14)
#ifndef PIN_IN1
#define PIN_IN1         7   // GPIO7 → TB67H IN1
#endif
#ifndef PIN_IN2
#define PIN_IN2         6   // GPIO6 → TB67H IN2
#endif

// ── Function output pins — BSS138 low-side switch, HIGH = load ON ─────────────
#define PIN_F1          0   // Lights
#define PIN_F2          1   // Horn
#define PIN_F3          2
#define PIN_F4          3

// ── Status / error LEDs ───────────────────────────────────────────────────────
#define PIN_ERROR_LED   5   // Red,    active HIGH
#define PIN_STATUS_LED  10  // Yellow, active HIGH

// ── Thermistor — ADC1_CH4 ─────────────────────────────────────────────────────
#define PIN_THERMISTOR  4

#define HORN_PULSE_MS   150

// ── State ─────────────────────────────────────────────────────────────────────
WiFiUDP udp;
unsigned long lastHeartbeat = 0;

int  motorSpeed = 0;    // 0–100
int  motorDir   = 1;    // 1=forward, 0=reverse
bool lightsOn   = false;
bool soundOn    = false;

// ── Motor control — TB67H450FNG ───────────────────────────────────────────────
// Stop/brake: IN1=H, IN2=H  →  brake LOW (both outputs pulled to GND)
// Forward:    IN1=H constant,  IN2=PWM  (duty 0→255 = speed 0→100%)
// Reverse:    IN2=H constant,  IN1=PWM  (duty 0→255 = speed 0→100%)
// L,L is never applied — would enter Hi-Z/standby after 1.5 ms
void applyMotor() {
    if (motorSpeed <= 0) {
        digitalWrite(PIN_IN1, HIGH);
        digitalWrite(PIN_IN2, HIGH);
        Serial.printf("Motor: BRAKE\n");
        return;
    }
    int duty = map(motorSpeed, 0, 100, 0, 255);
    if (motorDir == 1) {
        digitalWrite(PIN_IN1, HIGH);
        analogWrite(PIN_IN2, duty);
    } else {
        digitalWrite(PIN_IN2, HIGH);
        analogWrite(PIN_IN1, duty);
    }
    Serial.printf("Motor: speed=%d dir=%d duty=%d\n", motorSpeed, motorDir, duty);
}

// ── Parse "speed:dir:lights:sound" ───────────────────────────────────────────
void parseCommand(const String& payload) {
    int p1 = payload.indexOf(':');
    int p2 = payload.indexOf(':', p1 + 1);
    int p3 = payload.indexOf(':', p2 + 1);
    if (p1 < 0 || p2 < 0 || p3 < 0) {
        Serial.println("Bad payload: " + payload);
        return;
    }
    motorSpeed = payload.substring(0, p1).toInt();
    motorDir   = payload.substring(p1 + 1, p2).toInt();
    lightsOn   = payload.substring(p2 + 1, p3) == "true";
    soundOn    = payload.substring(p3 + 1) == "true";
    Serial.printf("CMD recv: speed=%d dir=%d lights=%d sound=%d\n",
                  motorSpeed, motorDir, (int)lightsOn, (int)soundOn);
    applyMotor();
    digitalWrite(PIN_F1, lightsOn ? HIGH : LOW);
    if (soundOn) {
        digitalWrite(PIN_F2, HIGH);
        delay(HORN_PULSE_MS);
        digitalWrite(PIN_F2, LOW);
    }
}

// ── Registration ──────────────────────────────────────────────────────────────
void registerWithServer() {
    String url = String("http://") + SPRING_HOST + ":" + SPRING_PORT
               + "/wifi/cab/register?ip=" + WiFi.localIP().toString()
               + "&name=" + CAB_NAME;
    Serial.println("Registering: " + url);

    bool registered = false;
    while (!registered) {
        HTTPClient http;
        http.begin(url);
        int code = http.POST("");
        if (code == 200) {
            Serial.println("Registered OK: " + http.getString());
            registered = true;
        } else {
            Serial.printf("Registration failed (HTTP %d), retrying in 5s...\n", code);
            digitalWrite(PIN_ERROR_LED, HIGH);
            delay(5000);
        }
        http.end();
    }
    digitalWrite(PIN_ERROR_LED, LOW);
    digitalWrite(PIN_STATUS_LED, HIGH);
}

// ── Heartbeat ─────────────────────────────────────────────────────────────────
void sendHeartbeat() {
    String url = String("http://") + SPRING_HOST + ":" + SPRING_PORT
               + "/wifi/cab/heartbeat?ip=" + WiFi.localIP().toString();

    HTTPClient http;
    http.begin(url);
    int code = http.GET();
    if (code == 200) {
        parseCommand(http.getString());
        Serial.println("Heartbeat OK");
        digitalWrite(PIN_ERROR_LED, LOW);
        digitalWrite(PIN_STATUS_LED, LOW);
        delay(80);
        digitalWrite(PIN_STATUS_LED, HIGH);
    } else if (code == 404) {
        Serial.println("Evicted — re-registering");
        registerWithServer();
    } else {
        Serial.printf("Heartbeat failed: HTTP %d\n", code);
        digitalWrite(PIN_ERROR_LED, HIGH);
    }
    http.end();
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) { delay(10); }

    // Motor — brake from first moment
    pinMode(PIN_IN1, OUTPUT);
    pinMode(PIN_IN2, OUTPUT);
    digitalWrite(PIN_IN1, HIGH);
    digitalWrite(PIN_IN2, HIGH);

    // Function outputs
    pinMode(PIN_F1, OUTPUT); digitalWrite(PIN_F1, LOW);
    pinMode(PIN_F2, OUTPUT); digitalWrite(PIN_F2, LOW);
    pinMode(PIN_F3, OUTPUT); digitalWrite(PIN_F3, LOW);
    pinMode(PIN_F4, OUTPUT); digitalWrite(PIN_F4, LOW);

    // LEDs
    pinMode(PIN_ERROR_LED,  OUTPUT); digitalWrite(PIN_ERROR_LED,  LOW);
    pinMode(PIN_STATUS_LED, OUTPUT); digitalWrite(PIN_STATUS_LED, LOW);

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Connecting to " WIFI_SSID);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nIP: " + WiFi.localIP().toString());

    registerWithServer();   // turns STATUS_LED on when done

    udp.begin(UDP_PORT);
    Serial.printf("UDP listening on port %d\n", UDP_PORT);
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    int pktLen = udp.parsePacket();
    if (pktLen > 0) {
        char buf[64];
        int n = udp.read(buf, sizeof(buf) - 1);
        buf[n] = '\0';
        parseCommand(String(buf));
    }

    if (millis() - lastHeartbeat >= HEARTBEAT_MS) {
        lastHeartbeat = millis();
        sendHeartbeat();
    }
}
