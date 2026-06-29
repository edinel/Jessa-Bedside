// Jessa-Bedside — Bluetooth A2DP speaker firmware
// Hardware: Adafruit ESP32 Feather V2 (#5400)
//           Adafruit TLV320DAC3100 I2S DAC + Class D amp (#6309)
//           Tang Band W2-2136S 2" full-range driver (4Ω)
//           WS2812B NeoPixel strip (inside enclosure, glow through finger joints)
//           Copper hex capacitive touch pad (one hex face panel)

#include <Adafruit_NeoPixel.h>
#include <Adafruit_TLV320DAC3100.h>
#include <Arduino.h>
#include <AudioTools.h>
#include <BluetoothA2DPSink.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include "arduino_secrets.h"

// ----------------------------------------------------------------------------
// Bluetooth device name — appears in phone's BT device list
// ----------------------------------------------------------------------------
#define DEVICE_NAME  "Jessa-Bedside"

// ----------------------------------------------------------------------------
// I2S pins → TLV320DAC3100
// I2S is the digital audio bus: BCK = bit clock, WSEL = left/right channel
// select (word select / LRCK), DIN = audio data into the DAC.
// These are software-assigned on ESP32 — any output-capable GPIO works.
// Prefixed I2S_ to avoid colliding with AudioTools' PIN_I2S_* macros.
// ----------------------------------------------------------------------------
#define I2S_PIN_BCK   27   // silkscreen: 27
#define I2S_PIN_WSEL  26   // silkscreen: A0
#define I2S_PIN_DIN   25   // silkscreen: A1

// RST: pull low briefly at startup to hardware-reset the TLV320 before
// initializing it over I2C. Without this, the chip may boot in an unknown state.
#define PIN_TLV_RST   33   // silkscreen: 33

// ----------------------------------------------------------------------------
// Capacitive touch pad
// ESP32 has built-in capacitive touch sensing — no external IC needed.
// touchRead() returns LOWER values when the pad is touched.
// A 1MΩ pulldown resistor between this pin and GND is required for reliable
// readings. Without it, the pin floats and gives erratic results.
// Calibration: print touchRead(PIN_TOUCH) to serial to find your thresholds.
// Typical values with this hardware: untouched ~1200, touched ~200.
// ----------------------------------------------------------------------------
#define PIN_TOUCH       4   // silkscreen: A5
#define TOUCH_THRESHOLD 400 // touched < threshold; untouched ~1200, touched ~200

// ----------------------------------------------------------------------------
// NeoPixel strip (WS2812B)
// Mounted inside the enclosure; warm amber light glows through the finger joints.
// PIN_PIXELS: data line to the strip's DIN pad.
// NUM_PIXELS: total LED count in the strip.
// PIXEL_BRIGHTNESS: global brightness scale, 0 (off) to 255 (full).
//   At full brightness a 58-pixel strip at 5V draws ~3.5A — way over budget.
//   Keep this low; 100 (~40%) is warm and easy on the 5V rail.
// PIXEL_COLOR: RGB color used when the light is on. Warm amber matches the
//   raw Baltic birch enclosure aesthetically.
// To switch back to the onboard NeoPixel for testing: PIN_PIXELS=0, NUM_PIXELS=1
// ----------------------------------------------------------------------------
#define PIN_PIXELS       14   // silkscreen: 14; change to 0 for onboard NeoPixel
#define NUM_PIXELS       58   // set to match your strip length
#define PIXEL_BRIGHTNESS 100  // 0–255; ~40% keeps current draw reasonable
#define PIXEL_COLOR      Adafruit_NeoPixel::Color(255, 180, 60)  // warm amber

// How often to poll the touch pad. 150ms debounces finger contact without
// feeling laggy. Polling faster than this causes spurious triggers.
static const unsigned long TOUCH_POLL_MS = 150;

// ----------------------------------------------------------------------------
// MQTT / Home Assistant
// Discovery: HA auto-creates the device from the config payload on first boot.
// State topic:   published after every on/off change (touch or MQTT command)
// Command topic: HA writes "ON" or "OFF" here to control the light remotely
// ----------------------------------------------------------------------------
#define MQTT_CLIENT_ID      "jessa_bedside"
#define MQTT_TOPIC_DISCOVER "homeassistant/light/jessa_bedside/config"
#define MQTT_TOPIC_STATE    "jessa_bedside/light/state"
#define MQTT_TOPIC_SET      "jessa_bedside/light/set"

#define MQTT_TOPIC_VOL_DISCOVER "homeassistant/number/jessa_bedside_volume/config"
#define MQTT_TOPIC_VOL_STATE    "jessa_bedside/volume/state"
#define MQTT_TOPIC_VOL_SET      "jessa_bedside/volume/set"

#define MQTT_TOPIC_BRIGHTNESS_STATE "jessa_bedside/light/brightness/state"
#define MQTT_TOPIC_BRIGHTNESS_SET   "jessa_bedside/light/brightness/set"

// How often the WiFi watchdog and MQTT reconnect logic run (milliseconds).
static const unsigned long WIFI_CHECK_MS = 30000;
static const unsigned long MQTT_CHECK_MS =  5000;

// ----------------------------------------------------------------------------
// Global objects
// ----------------------------------------------------------------------------

// NeoPixel strip object — constructor args: pixel count, data pin, LED type
Adafruit_NeoPixel      pixels(NUM_PIXELS, PIN_PIXELS, NEO_GRB + NEO_KHZ800);

// TLV320DAC3100 DAC/amp — communicates over I2C (SDA=GPIO22, SCL=GPIO20)
Adafruit_TLV320DAC3100 codec;

// I2SStream is the AudioTools I2S output driver. The A2DP sink writes decoded
// Bluetooth audio into this stream, which clocks it out over the I2S bus to
// the TLV320.
I2SStream              i2s;

// Bluetooth A2DP sink — presents as a Bluetooth audio receiver (like a speaker
// or headphones). The phone pairs once and reconnects automatically. Audio is
// decoded and routed directly into the I2SStream above.
// Heap-allocated in setup() to avoid a static-init crash in AudioTools::Allocator
// that occurs when this is constructed as a global before the ESP32 heap is ready.
BluetoothA2DPSink*     a2dp_sink = nullptr;

// MQTT client — wraps a WiFiClient for the PubSubClient transport layer
WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// Current LED state — toggled by touch or MQTT command
bool    g_leds_on       = false;
// Runtime brightness — initialised from the compile-time constant so the
// first power-on matches the hardware default.
uint8_t g_brightness    = PIXEL_BRIGHTNESS;
// DAC volume overlay in dB — sits on top of AVRCP phone volume passthrough
float   g_dac_volume_db = 0.0f;


// ----------------------------------------------------------------------------
// TLV320DAC3100 initialization
// The TLV320DAC3100 is NOT plug-and-play like simpler DACs (e.g. PCM5102A).
// It requires I2C configuration before any audio flows. The sequence below:
//   1. Hardware reset via RST pin
//   2. Configure I2S interface format (I2S, 16-bit)
//   3. Set up the PLL to derive the internal clock from the I2S bit clock (BCLK)
//   4. Configure DAC dividers so the output sample rate matches A2DP (44100 Hz)
//   5. Route DAC output to the analog mixer
//   6. Set digital volume to 0 dB (unity gain)
//   7. Enable and configure the Class D speaker amplifier
//   8. Enable and configure the headphone driver (for testing with the onboard jack)
// ----------------------------------------------------------------------------
void setupTLV320() {
    // Hardware reset: hold RST low for 100ms, then release. The TLV320
    // datasheet requires a reset pulse before the first I2C transaction.
    pinMode(PIN_TLV_RST, OUTPUT);
    digitalWrite(PIN_TLV_RST, LOW);
    delay(100);
    digitalWrite(PIN_TLV_RST, HIGH);

    if (!codec.begin()) {
        log_e("TLV320 not found");
        while (1) delay(10);  // halt — nothing works without the DAC
    }

    // I2S format: standard I2S (not left-justified or DSP mode), 16-bit words.
    // This must match the I2S config we set on the ESP32 side via AudioTools.
    if (!codec.setCodecInterface(TLV320DAC3100_FORMAT_I2S, TLV320DAC3100_DATA_LEN_16))
        log_e("TLV320: interface config failed");

    // Clock source: use the PLL, and feed the PLL from the I2S bit clock (BCLK).
    // BCLK is generated by the ESP32 as master; the TLV320 runs as I2S slave.
    if (!codec.setCodecClockInput(TLV320DAC3100_CODEC_CLKIN_PLL) ||
        !codec.setPLLClockInput(TLV320DAC3100_PLL_CLKIN_BCLK))
        log_e("TLV320: clock config failed");

    // PLL values: P=1, R=2, J=32, D=0
    // Formula: PLL_CLK = BCLK × (R × J) / P
    // At 44100 Hz stereo 16-bit: BCLK = 44100 × 32 = 1,411,200 Hz
    // PLL_CLK = 1,411,200 × (2 × 32) / 1 ≈ 90.3 MHz
    // Then: DAC_FS = PLL_CLK / (NDAC × MDAC × DOSR) = 90.3M / (8 × 2 × 128) = 44,100 Hz ✓
    if (!codec.setPLLValues(1, 2, 32, 0))
        log_e("TLV320: PLL values failed");

    // NDAC=8, MDAC=2: DAC clock dividers. Combined with DOSR=128 (default
    // processing block), these divide the PLL output down to 44100 Hz sample rate.
    if (!codec.setNDAC(true, 8) || !codec.setMDAC(true, 2))
        log_e("TLV320: DAC dividers failed");

    if (!codec.powerPLL(true))
        log_e("TLV320: PLL power-up failed");

    // Power up both DAC channels (left and right) with normal signal path.
    // VOLUME_STEP_1SAMPLE: volume changes ramp 1 step per sample (prevents clicks).
    if (!codec.setDACDataPath(true, true,
                              TLV320_DAC_PATH_NORMAL, TLV320_DAC_PATH_NORMAL,
                              TLV320_VOLUME_STEP_1SAMPLE))
        log_e("TLV320: DAC data path failed");

    // Route both DAC channels through the analog mixer to the output drivers.
    // The mixer feeds both the speaker (Class D) and headphone drivers.
    if (!codec.configureAnalogInputs(TLV320_DAC_ROUTE_MIXER, TLV320_DAC_ROUTE_MIXER,
                                     false, false, false, false))
        log_e("TLV320: analog routing failed");

    // Digital volume: 0.0 dB = unity gain. The function takes dB directly as a
    // float — passing a raw integer like 18 would mean +18 dB (extremely loud).
    if (!codec.setDACVolumeControl(false, false, TLV320_VOL_INDEPENDENT) ||
        !codec.setChannelVolume(false, 0.0f) ||
        !codec.setChannelVolume(true, 0.0f))
        log_e("TLV320: volume config failed");

    // Class D speaker amplifier: 6 dB gain (minimum available), unmuted.
    // setSPKVolume(true, 0): enable the speaker output path, 0 dB analog gain.
    if (!codec.enableSpeaker(true) ||
        !codec.configureSPK_PGA(TLV320_SPK_GAIN_6DB, true) ||
        !codec.setSPKVolume(true, 0))
        log_e("TLV320: speaker config failed");

    // Headphone driver: both channels powered up, standard common-mode voltage.
    // Useful for testing with the 3.5mm jack on the TLV320 board before the
    // speaker is installed. Remove if headphone output is not needed.
    if (!codec.configureHeadphoneDriver(true, true, TLV320_HP_COMMON_1_35V, false) ||
        !codec.configureHPL_PGA(0, true) ||
        !codec.configureHPR_PGA(0, true) ||
        !codec.setHPLVolume(true, 6) ||
        !codec.setHPRVolume(true, 6))
        log_e("TLV320: headphone config failed");

    log_i("TLV320 OK");
}


// ----------------------------------------------------------------------------
// LED rendering
// Always sets brightness then re-fills from the source color constant before
// calling show(). This prevents compounding: setBrightness() scales whatever
// is in the pixel buffer in-place, so calling it twice without re-filling
// produces non-linear results.
// ----------------------------------------------------------------------------
static void renderLEDs() {
    pixels.setBrightness(g_brightness);
    if (g_leds_on) {
        pixels.fill(PIXEL_COLOR);
    } else {
        pixels.clear();
    }
    pixels.show();
}

// Single entry point for on/off state changes.
void setLEDs(bool on) {
    g_leds_on = on;
    renderLEDs();
    if (mqtt.connected()) {
        mqtt.publish(MQTT_TOPIC_STATE, on ? "ON" : "OFF", /*retain=*/true);
    }
    log_i("LEDs %s", on ? "on" : "off");
}

// Single entry point for brightness changes.
// If the light is off the new brightness is stored but the strip stays dark;
// it takes effect on the next power-on.
void setBrightnessLevel(uint8_t brightness) {
    g_brightness = brightness;
    renderLEDs();
    if (mqtt.connected()) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%u", brightness);
        mqtt.publish(MQTT_TOPIC_BRIGHTNESS_STATE, buf, /*retain=*/true);
    }
    log_i("Brightness: %u", brightness);
}


// ----------------------------------------------------------------------------
// DAC volume overlay
// Applies a dB offset to both DAC channels independently of the AVRCP phone
// volume passthrough. Clamped to the hardware range before writing.
// ----------------------------------------------------------------------------
void setDACVolume(float db) {
    db = constrain(db, -20.0f, 20.0f);
    g_dac_volume_db = db;
    codec.setChannelVolume(false, db);  // left
    codec.setChannelVolume(true,  db);  // right

    if (mqtt.connected()) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%.1f", db);
        mqtt.publish(MQTT_TOPIC_VOL_STATE, buf, /*retain=*/true);
    }
    log_i("DAC volume: %.1f dB", db);
}


// ----------------------------------------------------------------------------
// Capacitive touch polling
// Called every loop iteration. Rate-limited to TOUCH_POLL_MS to debounce.
// Detects the leading edge of a touch (finger down) and toggles the LEDs.
// Holding your finger on the pad does NOT keep toggling — only the initial
// contact triggers the action.
// ----------------------------------------------------------------------------
void checkTouch() {
    static unsigned long lastPoll    = 0;
    static bool          lastTouched = false;

    unsigned long now = millis();
    if (now - lastPoll < TOUCH_POLL_MS) return;
    lastPoll = now;

    // touchRead returns lower values when the pad is touched (higher capacitance)
    bool touched = (touchRead(PIN_TOUCH) < TOUCH_THRESHOLD);

    // Only act on the transition from not-touched to touched (leading edge)
    if (touched && !lastTouched) {
        setLEDs(!g_leds_on);
    }
    lastTouched = touched;
}


// ----------------------------------------------------------------------------
// AVRCP volume control
// Called by the A2DP library when the connected device changes its volume.
// Maps the Bluetooth volume (0–127) directly to the A2DP sink's software
// volume, so the phone's volume slider controls the speaker output level.
// ----------------------------------------------------------------------------
void onVolumeChanged(int volume) {
    a2dp_sink->set_volume(volume);
    log_i("Volume: %d", volume);
}


// ----------------------------------------------------------------------------
// MQTT message callback
// Called by PubSubClient when a message arrives on a subscribed topic.
// Only MQTT_TOPIC_SET is subscribed; the payload is "ON" or "OFF".
// ----------------------------------------------------------------------------
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
    String msg((char*)payload, length);

    if (strcmp(topic, MQTT_TOPIC_SET) == 0) {
        if (msg == "ON")       setLEDs(true);
        else if (msg == "OFF") setLEDs(false);
    } else if (strcmp(topic, MQTT_TOPIC_BRIGHTNESS_SET) == 0) {
        setBrightnessLevel((uint8_t)constrain(msg.toInt(), 0, 255));
    } else if (strcmp(topic, MQTT_TOPIC_VOL_SET) == 0) {
        setDACVolume(msg.toFloat());
    }
}


// ----------------------------------------------------------------------------
// MQTT connect + Home Assistant discovery
// Publishes a discovery payload so HA auto-creates a "light" entity for the
// NeoPixel strip. Discovery only needs to happen once per broker session —
// HA caches it. The retain flag ensures it survives broker restarts.
// ----------------------------------------------------------------------------
void connectMQTT() {
    log_i("MQTT connecting to %s …", mqttServer);

    if (!mqtt.connect(MQTT_CLIENT_ID, mqttUser, mqttPass,
                      MQTT_TOPIC_STATE, /*qos*/0, /*retain*/true, "OFF")) {
        log_w("MQTT connect failed, rc=%d", mqtt.state());
        return;
    }

    log_i("MQTT connected");

    // HA MQTT discovery payload — HA parses this and creates a light entity.
    // unique_id ties the entity to this device across renames/re-pairs.
    const char* discovery =
        "{"
          "\"name\":\"Jessa Bedside\","
          "\"unique_id\":\"jessa_bedside_light\","
          "\"device\":{"
            "\"identifiers\":[\"jessa_bedside\"],"
            "\"name\":\"Jessa Bedside\","
            "\"model\":\"ESP32 Bedside Speaker\","
            "\"manufacturer\":\"DIY\""
          "},"
          "\"state_topic\":\"" MQTT_TOPIC_STATE "\","
          "\"command_topic\":\"" MQTT_TOPIC_SET "\","
          "\"brightness_state_topic\":\"" MQTT_TOPIC_BRIGHTNESS_STATE "\","
          "\"brightness_command_topic\":\"" MQTT_TOPIC_BRIGHTNESS_SET "\","
          "\"brightness_scale\":255,"
          "\"payload_on\":\"ON\","
          "\"payload_off\":\"OFF\","
          "\"optimistic\":false"
        "}";
    mqtt.publish(MQTT_TOPIC_DISCOVER, discovery, /*retain=*/true);

    mqtt.subscribe(MQTT_TOPIC_SET);
    mqtt.subscribe(MQTT_TOPIC_BRIGHTNESS_SET);

    const char* volDiscovery =
        "{"
          "\"name\":\"Jessa Bedside Volume\","
          "\"unique_id\":\"jessa_bedside_volume\","
          "\"device\":{\"identifiers\":[\"jessa_bedside\"]},"
          "\"state_topic\":\"" MQTT_TOPIC_VOL_STATE "\","
          "\"command_topic\":\"" MQTT_TOPIC_VOL_SET "\","
          "\"min\":-20,"
          "\"max\":20,"
          "\"step\":0.5,"
          "\"unit_of_measurement\":\"dB\""
        "}";
    mqtt.publish(MQTT_TOPIC_VOL_DISCOVER, volDiscovery, /*retain=*/true);

    mqtt.subscribe(MQTT_TOPIC_VOL_SET);

    // Publish current states so HA is immediately in sync after (re)connect
    mqtt.publish(MQTT_TOPIC_STATE, g_leds_on ? "ON" : "OFF", /*retain=*/true);

    char brightBuf[4];
    snprintf(brightBuf, sizeof(brightBuf), "%u", g_brightness);
    mqtt.publish(MQTT_TOPIC_BRIGHTNESS_STATE, brightBuf, /*retain=*/true);

    char volBuf[8];
    snprintf(volBuf, sizeof(volBuf), "%.1f", g_dac_volume_db);
    mqtt.publish(MQTT_TOPIC_VOL_STATE, volBuf, /*retain=*/true);
}


// ----------------------------------------------------------------------------
// WiFi watchdog — called from loop every WIFI_CHECK_MS milliseconds.
// A startup-only connection attempt is not sufficient; the WiFi subsystem
// stops responding over time without a periodic reconnect cycle.
//
// Non-blocking by design: WiFi.begin() hands off to the ESP32 radio stack
// and returns immediately. The next WIFI_CHECK_MS interval will see whether
// it connected. This keeps checkTouch() running at all times — the light
// must never be held hostage by networking.
// ----------------------------------------------------------------------------
void checkWiFi() {
    static unsigned long lastCheck = 0;
    unsigned long now = millis();
    if (now - lastCheck < WIFI_CHECK_MS) return;
    lastCheck = now;

    if (WiFi.status() == WL_CONNECTED) return;

    log_w("WiFi disconnected — reconnecting …");
    WiFi.disconnect();
    WiFi.begin(ssid, pass);
    // checkMQTT() will detect the new connection and (re)connect the broker.
}


// ----------------------------------------------------------------------------
// MQTT watchdog — called from loop every MQTT_CHECK_MS milliseconds.
// ----------------------------------------------------------------------------
void checkMQTT() {
    static unsigned long lastCheck = 0;
    unsigned long now = millis();
    if (now - lastCheck < MQTT_CHECK_MS) return;
    lastCheck = now;

    if (WiFi.status() != WL_CONNECTED) return;  // wait for WiFi first
    if (mqtt.connected()) return;

    connectMQTT();
}


// ----------------------------------------------------------------------------
// Setup
// ----------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(2000);  // give USB CDC time to enumerate before any serial output

    // Construct the A2DP sink here rather than as a global — AudioTools'
    // Allocator crashes if constructed before the ESP32 heap is ready.
    a2dp_sink = new BluetoothA2DPSink(i2s);

    // Initialize NeoPixels before anything else so the strip starts dark.
    // setBrightness sets a global scale factor applied to all pixel writes.
    pixels.begin();
    setLEDs(false);  // ensure strip starts off; renderLEDs() sets brightness

    // Initialize the TLV320DAC3100 over I2C. This must happen before starting
    // the I2S stream, otherwise the DAC may latch on to the I2S clock in an
    // unconfigured state and produce noise or silence.
    setupTLV320();

    // Configure the I2S output stream (AudioTools). TX_MODE = transmit only.
    // These pins must match the physical wiring to the TLV320DAC3100.
    auto cfg      = i2s.defaultConfig(TX_MODE);
    cfg.pin_bck   = I2S_PIN_BCK;
    cfg.pin_ws    = I2S_PIN_WSEL;
    cfg.pin_data  = I2S_PIN_DIN;
    i2s.begin(cfg);

    // Register the AVRCP volume callback before starting the sink, so volume
    // events from the phone are handled immediately on connection.
    a2dp_sink->set_avrc_rn_volumechange(onVolumeChanged);

    // Start the Bluetooth A2DP sink. The device will appear as DEVICE_NAME
    // in the phone's Bluetooth device list. After first pairing, the phone
    // reconnects automatically whenever both devices are powered on.
    a2dp_sink->start(DEVICE_NAME);
    log_i("A2DP sink started: %s", DEVICE_NAME);

    // Kick off WiFi association and return immediately — the watchdog in loop()
    // handles both the initial connection and any later drops. The light must
    // be fully interactive before WiFi is up.
    log_i("WiFi connecting to %s …", ssid);
    WiFi.begin(ssid, pass);

    // Configure MQTT broker and register the incoming-message callback.
    // Buffer must be large enough for the discovery payloads (~450 bytes);
    // the default 256 causes publish() to silently fail on oversized packets.
    mqtt.setBufferSize(768);
    mqtt.setSocketTimeout(1);  // 1 s max block per connect attempt — local broker is fast
    mqtt.setServer(mqttServer, 1883);
    mqtt.setCallback(onMqttMessage);
}


// ----------------------------------------------------------------------------
// Loop
// The A2DP library runs on its own FreeRTOS tasks — no audio polling needed
// here. The loop handles touch input, WiFi/MQTT watchdogs, and the MQTT
// receive pump (mqtt.loop()).
// ----------------------------------------------------------------------------
void loop() {
    checkWiFi();
    checkMQTT();
    mqtt.loop();     // pumps incoming MQTT messages to onMqttMessage()
    checkTouch();
}
