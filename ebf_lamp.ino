// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Kirill Levo (cookobit)
// EBF Lamp Firmware
// XIAO ESP32-S3
// Normal mode: CCT lamp with 3 profiles + pot-controlled warm/cold mix.
// Furnace mode: 5 rapid taps fires up the EBF — slow ramp to peak,
//               subtle breathe at operating temperature, 5 more taps cools down.
//               A third 5-tap during cooldown snaps fully off.

// ── Debug ─────────────────────────────────────────────────────
#define DEBUG true

// ── Pins ─────────────────────────────────────────────────────
#define PIN_POT       2
#define PIN_LED_GREEN 5
#define PIN_LED_RED   4
#define PIN_PWM_WARM  7
#define PIN_PWM_COLD  8
#define PIN_TOUCH     43

// ── PWM ──────────────────────────────────────────────────────
#define PWM_FREQ      1000
#define PWM_RES       8

// ── Profiles (gamma-corrected for perceptual linearity) ──────
const uint8_t PROFILES[3] = {
  255,   // Profile 1: 100% perceived
   127,   // Profile 2:  50% perceived
   25    // Profile 3:  25% perceived
};

// ── Timing ───────────────────────────────────────────────────
#define PROFILE_WINDOW_MS    2000
#define DEBOUNCE_MS          50

// ── Furnace mode ─────────────────────────────────────────────
#define TURBO_TAP_COUNT          5
#define TURBO_TAP_WINDOW_MS      1500
#define HEAT_UP_MS               15000   // 15 s heat-up ramp
#define COOL_DOWN_MS             3000    //  3 s snappy cool-down
#define BREATHE_PERIOD_MS        4000    // operational breathing cycle
#define OPERATIONAL_TIMEOUT_MS   (10UL * 60UL * 1000UL)  // 10 min auto-cool
#define BREATHE_DEPTH            0.15f   // ±15% pulse around peak
#define FURNACE_PEAK_WARM        255     // Cupronickel orange — saturated
#define FURNACE_PEAK_COLD        128     // adds flux without killing the warmth

// ── State ────────────────────────────────────────────────────
enum FurnaceState {
  STATE_NORMAL,
  STATE_HEATING,
  STATE_OPERATIONAL,
  STATE_COOLING
};

bool          lampOn          = false;
uint8_t       profileIndex    = 0;
bool          lastTouchState  = false;
unsigned long offTime         = 0;
unsigned long lastDebounce    = 0;

FurnaceState  furnaceState    = STATE_NORMAL;
unsigned long stateStartTime  = 0;
uint8_t       savedProfile    = 0;
bool          savedLampOn     = false;

unsigned long tapTimes[TURBO_TAP_COUNT] = {0};
uint8_t       tapHead = 0;

// ── Helpers ──────────────────────────────────────────────────

void getPWMValues(uint8_t &warmDuty, uint8_t &coldDuty) {
  int adcVal = analogRead(PIN_POT);
  float t = adcVal / 4095.0f;
  uint8_t profile = PROFILES[profileIndex];
  warmDuty = (uint8_t)((1.0f - t) * profile);
  coldDuty = (uint8_t)(t * profile);
}

void recordTap(unsigned long now) {
  tapTimes[tapHead] = now;
  tapHead = (tapHead + 1) % TURBO_TAP_COUNT;
}

bool isTurboSequence(unsigned long now) {
  unsigned long oldest = tapTimes[tapHead];
  if (oldest == 0) return false;
  return (now - oldest) <= TURBO_TAP_WINDOW_MS;
}

void clearTapHistory() {
  for (uint8_t i = 0; i < TURBO_TAP_COUNT; i++) tapTimes[i] = 0;
  tapHead = 0;
}

void applyLampNormal() {
  if (!lampOn) {
    ledcWrite(PIN_PWM_WARM, 0);
    ledcWrite(PIN_PWM_COLD, 0);
    digitalWrite(PIN_LED_GREEN, LOW);
    digitalWrite(PIN_LED_RED,   HIGH);
  } else {
    uint8_t w, c;
    getPWMValues(w, c);
    ledcWrite(PIN_PWM_WARM, w);
    ledcWrite(PIN_PWM_COLD, c);
    digitalWrite(PIN_LED_GREEN, HIGH);
    digitalWrite(PIN_LED_RED,   LOW);
  }
}

// Apply furnace state, called every loop iteration when active
void applyFurnaceState() {
  unsigned long now     = millis();
  unsigned long elapsed = now - stateStartTime;
  float totalNorm = 0.0f;

  switch (furnaceState) {
    case STATE_HEATING: {
      float progress = (float)elapsed / HEAT_UP_MS;
      if (progress >= 1.0f) {
        furnaceState   = STATE_OPERATIONAL;
        stateStartTime = now;
        if (DEBUG) Serial.println("→ Furnace at operating temperature");
        progress = 1.0f;
      }
      // Smoothstep S-curve: anticipation -> ramp -> tapering
      totalNorm = progress * progress * (3.0f - 2.0f * progress);
      break;
    }

    case STATE_OPERATIONAL: {
      // Slow breathing around peak, active EBF processing
      float phase = (float)(elapsed % BREATHE_PERIOD_MS) / BREATHE_PERIOD_MS;
      phase *= 2.0f * PI;
      totalNorm = 1.0f - (BREATHE_DEPTH * 0.5f * (1.0f - cos(phase)));

      // Auto-cool after timeout
      if (elapsed >= OPERATIONAL_TIMEOUT_MS) {
        furnaceState   = STATE_COOLING;
        stateStartTime = now;
        if (DEBUG) Serial.println("→ Furnace cooling (auto-timeout)");
      }
      break;
    }

    case STATE_COOLING: {
      float progress = (float)elapsed / COOL_DOWN_MS;
      if (progress >= 1.0f) {
        // Return to where the user was before firing up
        furnaceState = STATE_NORMAL;
        profileIndex = savedProfile;
        lampOn       = savedLampOn;
        clearTapHistory();
        applyLampNormal();
        if (DEBUG) Serial.println("→ Furnace cold");
        return;
      }
      float remaining = 1.0f - progress;
      totalNorm = remaining * remaining;
      break;
    }

    default:
      return;
  }

  // Drive each channel against its own peak, scaled by totalNorm
  uint8_t warmDuty = (uint8_t)(totalNorm * FURNACE_PEAK_WARM);
  uint8_t coldDuty = (uint8_t)(totalNorm * FURNACE_PEAK_COLD);

  ledcWrite(PIN_PWM_WARM, warmDuty);
  ledcWrite(PIN_PWM_COLD, coldDuty);
  digitalWrite(PIN_LED_GREEN, HIGH);
  digitalWrite(PIN_LED_RED,   HIGH);   // both indicators ON = furnace mode
}

void enterHeating() {
  savedProfile   = profileIndex;
  savedLampOn    = lampOn;
  furnaceState   = STATE_HEATING;
  stateStartTime = millis();
  clearTapHistory();
  if (DEBUG) Serial.println("→ EBF firing up");
}

void enterCooling() {
  furnaceState   = STATE_COOLING;
  stateStartTime = millis();
  clearTapHistory();
  if (DEBUG) Serial.println("→ EBF cooling down");
}

void handleTouch() {
  unsigned long now = millis();
  recordTap(now);

  if (isTurboSequence(now)) {
    switch (furnaceState) {
      case STATE_NORMAL:
        enterHeating();
        break;
      case STATE_HEATING:
      case STATE_OPERATIONAL:
        enterCooling();
        break;
      case STATE_COOLING:
        // Second 5-tap during cooldown → snap to fully off
        furnaceState = STATE_NORMAL;
        profileIndex = savedProfile;
        lampOn       = savedLampOn;
        clearTapHistory();
        applyLampNormal();
        if (DEBUG) Serial.println("→ Cooldown skipped, fully off");
        break;
    }
    return;
  }

  // Swallow individual taps while in furnace mode
  if (furnaceState != STATE_NORMAL) return;

  // Normal lamp logic
  if (lampOn) {
    lampOn = false;
    offTime = now;
    applyLampNormal();
    if (DEBUG) Serial.println("→ Lamp OFF");
  } else {
    if ((now - offTime) <= PROFILE_WINDOW_MS) {
      profileIndex = (profileIndex + 1) % 3;
      if (DEBUG) Serial.println("→ Profile advanced");
    } else {
      profileIndex = 0;
      if (DEBUG) Serial.println("→ Profile reset");
    }
    lampOn = true;
    applyLampNormal();
    if (DEBUG) {
      Serial.print("→ Lamp ON, Profile ");
      Serial.print(profileIndex + 1);
      Serial.print(" (");
      Serial.print((PROFILES[profileIndex] * 100) / 255);
      Serial.println("%)");
    }
  }
}

// ── Setup ────────────────────────────────────────────────────
void setup() {
  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_RED,   OUTPUT);
  pinMode(PIN_TOUCH, INPUT);
  analogReadResolution(12);
  ledcAttach(PIN_PWM_WARM, PWM_FREQ, PWM_RES);
  ledcAttach(PIN_PWM_COLD, PWM_FREQ, PWM_RES);
  applyLampNormal();

  if (DEBUG) {
    Serial.begin(115200);
    Serial.println("EBF Lamp — DEBUG mode");
    Serial.println("State | Profile | ADC  | Warm | Cold");
    Serial.println("------+---------+------+------+-----");
  }
}

// ── Main loop ────────────────────────────────────────────────
void loop() {
  bool touchNow = digitalRead(PIN_TOUCH);
  unsigned long now = millis();

  if (touchNow && !lastTouchState && (now - lastDebounce) > DEBOUNCE_MS) {
    lastDebounce = now;
    handleTouch();
  }
  lastTouchState = touchNow;

  // Drive outputs
  if (furnaceState != STATE_NORMAL) {
    applyFurnaceState();
  } else if (lampOn) {
    uint8_t w, c;
    getPWMValues(w, c);
    ledcWrite(PIN_PWM_WARM, w);
    ledcWrite(PIN_PWM_COLD, c);
  }

  // Debug print
  if (DEBUG) {
    static unsigned long lastPrint = 0;
    if (millis() - lastPrint >= 250) {
      lastPrint = millis();
      int adcVal = analogRead(PIN_POT);
      uint8_t w, c;
      getPWMValues(w, c);

      const char* stateStr;
      switch (furnaceState) {
        case STATE_HEATING:     stateStr = "HEAT"; break;
        case STATE_OPERATIONAL: stateStr = "OPER"; break;
        case STATE_COOLING:     stateStr = "COOL"; break;
        default:                stateStr = lampOn ? "ON  " : "OFF "; break;
      }
      Serial.print(stateStr);
      Serial.print("  | P");
      Serial.print(profileIndex + 1);
      Serial.print("      | ");
      Serial.print(adcVal);
      Serial.print(" | ");
      Serial.print(w);
      Serial.print("   | ");
      Serial.println(c);
    }
  }

  delay(10);
}