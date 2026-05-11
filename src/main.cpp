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

// ── Motor pins — TLE5206 IN1/IN2 (XIAO D10/D9 = GPIO10/GPIO9, per schematic) ─
#define PIN_IN1         10
#define PIN_IN2         9
#define PWM_CHANNEL_IN1 0
#define PWM_CHANNEL_IN2 1
#define PWM_FREQ        1000
#define PWM_RES         8   // 8-bit: 0-255

// ── State ─────────────────────────────────────────────────────────────────────
WiFiUDP udp;
unsigned long lastHeartbeat = 0;

int  motorSpeed = 0;    // 0-126
int  motorDir   = 1;    // 1=forward 0=reverse
bool lightsOn   = false;
bool soundOn    = false;

// ── Motor control ─────────────────────────────────────────────────────────────
void applyMotor() {
    int duty = map(motorSpeed, 0, 126, 0, 255);
    if (motorDir == 1) {
        ledcWrite(PWM_CHANNEL_IN1, duty);
        ledcWrite(PWM_CHANNEL_IN2, 0);
    } else {
        ledcWrite(PWM_CHANNEL_IN1, 0);
        ledcWrite(PWM_CHANNEL_IN2, duty);
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
                  motorSpeed, motorDir, lightsOn, soundOn);
    applyMotor();
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

    ledcSetup(PWM_CHANNEL_IN1, PWM_FREQ, PWM_RES);
    ledcAttachPin(PIN_IN1, PWM_CHANNEL_IN1);
    ledcSetup(PWM_CHANNEL_IN2, PWM_FREQ, PWM_RES);
    ledcAttachPin(PIN_IN2, PWM_CHANNEL_IN2);
    ledcWrite(PWM_CHANNEL_IN1, 0);
    ledcWrite(PWM_CHANNEL_IN2, 0);

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
