#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiUDP.h>

// ── Configuration ─────────────────────────────────────────────────────────────
#define WIFI_SSID       "toytrains"
#define WIFI_PASS       "toytrain"
#define SPRING_HOST     "10.42.0.1"   // Pi's IP on the toytrains AP (wlan1)
#define SPRING_PORT     80
#define CAB_NAME        "WiFiLoco1"

#define UDP_PORT        4210
#define HEARTBEAT_MS    30000

// ── Motor pins — TLE5206 IN1/IN2 via level shifters ──────────────────────────
// Defaults for rev2 PCB (Super Mini ESP32-C3); wroom32 env overrides via build_flags
#ifndef PIN_IN1
#define PIN_IN1         7
#endif
#ifndef PIN_IN2
#define PIN_IN2         6
#endif

// ── Function pins — BSS138 low-side GND switch, HIGH = ON ─────────────────────
#define PIN_LIGHTS      2
#define PIN_HORN        3
#define HORN_PULSE_MS   150

// ── State ─────────────────────────────────────────────────────────────────────
WiFiUDP udp;
unsigned long lastHeartbeat = 0;

int  motorSpeed = 0;    // 0-126
int  motorDir   = 1;    // 1=forward 0=reverse
bool lightsOn   = false;
bool soundOn    = false;

// ── Motor control — locked anti-phase (LAP) drive, mirrors DccDecoder_TLE5206 ─
// Stop:    IN1=H, IN2=H → locked brake (both high-side, OUT=VS, no current)
// Forward: IN2=PWM 127→255 as speed 0→100, IN1=LOW
// Reverse: IN2=PWM 127→0 as speed 0→100, IN1=LOW
void applyMotor() {
    if (motorSpeed <= 0) {
        digitalWrite(PIN_IN1, HIGH);
        digitalWrite(PIN_IN2, HIGH);
        Serial.printf("Motor: speed=%d dir=%d duty=BRAKE\n", motorSpeed, motorDir);
        return;
    }
    int duty;
    if (motorDir == 1) {
        duty = map(motorSpeed, 0, 100, 127, 0);
    } else {
        duty = map(motorSpeed, 0, 100, 127, 255);
    }
    analogWrite(PIN_IN2, duty);
    digitalWrite(PIN_IN1, LOW);
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
                  motorSpeed, motorDir, lightsOn, soundOn);
    applyMotor();
    digitalWrite(PIN_LIGHTS, lightsOn ? HIGH : LOW);
    if (soundOn) {
        digitalWrite(PIN_HORN, HIGH);
        delay(HORN_PULSE_MS);
        digitalWrite(PIN_HORN, LOW);
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
            delay(5000);
        }
        http.end();
    }
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
    } else if (code == 404) {
        Serial.println("Evicted — re-registering");
        registerWithServer();
    } else {
        Serial.printf("Heartbeat failed: HTTP %d\n", code);
    }
    http.end();
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000) { delay(10); }

    pinMode(PIN_IN1, OUTPUT);
    pinMode(PIN_IN2, OUTPUT);
    digitalWrite(PIN_IN1, HIGH);
    digitalWrite(PIN_IN2, HIGH);   // locked brake from the first moment

    pinMode(PIN_LIGHTS, OUTPUT);
    pinMode(PIN_HORN, OUTPUT);
    digitalWrite(PIN_LIGHTS, LOW);
    digitalWrite(PIN_HORN, LOW);

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Connecting to " WIFI_SSID);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nIP: " + WiFi.localIP().toString());

    registerWithServer();

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
