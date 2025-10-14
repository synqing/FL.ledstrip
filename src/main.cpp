#include <Arduino.h>
#include <FastLED.h>
#include <Wire.h>
#include <math.h>
#include "config/features.h"
#include "config/hardware_config.h"
#include "utils/PerformanceMetrics.h"
#include "utils/FastLEDLUTs.h"

#if FEATURE_ENCODERS
#include "m5rotate8.h"
#endif

// LED configuration
static constexpr uint16_t NUM_LEDS = HardwareConfig::NUM_LEDS;
static constexpr uint8_t MATRIX_WIDTH = HardwareConfig::MATRIX_WIDTH;
static constexpr uint8_t MATRIX_HEIGHT = HardwareConfig::MATRIX_HEIGHT;
static constexpr uint8_t DATA_PIN = HardwareConfig::LED_DATA_PIN;
static constexpr uint8_t BUTTON_PIN_GPIO = HardwareConfig::BUTTON_PIN;
static constexpr uint8_t POWER_PIN_GPIO = HardwareConfig::POWER_PIN;

#define LED_TYPE WS2812
#define COLOR_ORDER GRB
// Behaviour knobs
static const uint32_t PALETTE_DWELL_MS = 8000;      // time per palette when in effect mode
static const uint32_t PALETTE_MODE_DWELL_MS = 3000; // faster palette showcase cadence
static const bool PALETTE_RANDOMISE = false;        // random order if true
static const bool APPLY_GAMMA = true;               // apply video gamma after drawing
static const float GAMMA_VALUE = 2.2f;              // gamma value
static const uint16_t MAX_CURRENT_MA = 2000;        // power limit
static const bool ENABLE_LINEARISE_PALETTES = false; // sRGB->linear palette entries
static const bool ENABLE_BROWN_GUARDRAIL = false;   // clamp G,B relative to R for brown/orange region
static const uint8_t MAX_G_PERCENT_OF_R = 28;       // G <= 28% of R
static const uint8_t MAX_B_PERCENT_OF_R = 8;        // B <= 8% of R
static const bool ENABLE_AUTO_EXPOSURE = false;     // downscale if too bright
static const uint8_t AE_TARGET_LUMA = 110;          // target average luma (0-255)

static constexpr uint8_t MIN_BRIGHTNESS = 8;
static constexpr uint8_t MAX_BRIGHTNESS = 160;
static constexpr uint8_t BRIGHTNESS_STEP = 4;

// NEW: post-filters
static const bool ENABLE_BLUR2D = false;         // 2D blur to hide banding
static const uint8_t BLUR2D_AMOUNT = 24;         // 0..255 fract8
static const bool ENABLE_VIGNETTE = false;       // radial vignette using radii LUT
static const uint8_t VIGNETTE_STRENGTH = 64;     // 0..255 (higher = darker edges)

// NEW: palette curation (baked trim beyond guardrail)
static const bool ENABLE_PALETTE_CURATION = false; // curate palette entries when selected
static const uint8_t CURATION_GREEN_SCALE = 230;  // default green scale8 for all palettes
static const uint8_t CURATION_BROWN_GREEN_SCALE = 190; // stronger green trim for brown-heavy palettes

// sRGB -> linear LUT and helpers
static uint8_t gSRGB2LinearLUT[256];
static inline uint8_t srgb2linear(uint8_t c) { return gSRGB2LinearLUT[c]; }
static void initSRGB2LinearLUT() {
    for (uint16_t i = 0; i < 256; ++i) {
        float srgb = static_cast<float>(i) / 255.0f;
        float linear = (srgb <= 0.04045f)
                           ? srgb / 12.92f
                           : powf((srgb + 0.055f) / 1.055f, 2.4f);
        float scaled = linear * 255.0f + 0.5f;
        if (scaled < 0.0f) scaled = 0.0f;
        if (scaled > 255.0f) scaled = 255.0f;
        gSRGB2LinearLUT[i] = static_cast<uint8_t>(scaled);
    }
}
static inline void linearisePalette(CRGBPalette16 &pal) {
    for (uint8_t i = 0; i < 16; ++i) {
        CRGB &c = pal.entries[i];
        c.r = srgb2linear(c.r);
        c.g = srgb2linear(c.g);
        c.b = srgb2linear(c.b);
    }
}

// NEW: palette curation helpers
static inline bool isBrownish(const CRGB &c) { return (c.r > c.g) && (c.g >= c.b); }
static inline void curatePalette(CRGBPalette16 &pal) {
    if (!ENABLE_PALETTE_CURATION) return;
    uint8_t brownishCount = 0;
    for (uint8_t i = 0; i < 16; ++i) if (isBrownish(pal.entries[i])) brownishCount++;
    uint8_t gScale = (brownishCount >= 6) ? CURATION_BROWN_GREEN_SCALE : CURATION_GREEN_SCALE;
    for (uint8_t i = 0; i < 16; ++i) {
        pal.entries[i].g = scale8(pal.entries[i].g, gScale);
    }
}

// NEW: vignette helper
static inline void applyVignette(CRGB *buf) {
    extern uint8_t radii[NUM_LEDS];
    if (!ENABLE_VIGNETTE) return;
    for (uint8_t i = 0; i < NUM_LEDS; ++i) {
        uint8_t r = radii[i]; // 0..255 from LUT
        uint8_t scale = 255 - scale8(r, VIGNETTE_STRENGTH);
        buf[i].nscale8_video(scale);
    }
}


// Global LED array
CRGB leds[NUM_LEDS];
CRGB transitionBuffer[NUM_LEDS];  // For smooth transitions

// Strip mapping arrays for spatial effects
uint8_t angles[NUM_LEDS];
uint8_t radii[NUM_LEDS];

#if FEATURE_ENCODERS
static void initializeEncoderHardware();
static void handleEncoderInput();
static void restoreEncoderLed(uint8_t index);
static void flashEncoderLed(uint8_t index, uint8_t r, uint8_t g, uint8_t b, uint16_t durationMs);
#endif

// Effect parameters
uint8_t gHue = 0;
uint8_t currentEffect = 0;
uint8_t previousEffect = 0;
uint32_t lastButtonPress = 0;
uint8_t fadeAmount = 20;
uint8_t paletteSpeed = 10;
uint8_t currentBrightness = HardwareConfig::DEFAULT_BRIGHTNESS;
bool paletteMode = false;
bool paletteModeChanged = false;
uint32_t lastPaletteChangeMs = 0;

#if FEATURE_ENCODERS
static M5ROTATE8 encoder(M5ROTATE8_DEFAULT_ADDRESS, &Wire);
static bool encoderConnected = false;
static bool encoderButtonState[8] = {false};
static uint32_t encoderLedFlashUntil[8] = {0};
static uint32_t lastEncoderReconnectAttempt = 0;
#endif

// Transition parameters
bool inTransition = false;
uint32_t transitionStart = 0;
uint32_t transitionDuration = 1000;  // 1 second transitions
uint8_t transitionProgress = 0;  // 0-255 instead of float

// Palette management
CRGBPalette16 currentPalette;
CRGBPalette16 targetPalette;
uint16_t currentPaletteIndex = 0; // combined index (Standard + Crameri)
uint16_t totalPaletteCount = 0;

// Include palette definitions (Standard) and Crameri
#include "Palettes.h"
#include "CrameriPalettes.h"

// Performance monitoring
PerformanceMetrics metrics;

// Effect function pointer type
typedef void (*EffectFunction)();

struct Effect {
    const char* name;
    EffectFunction function;
};

extern Effect effects[];
extern const uint8_t NUM_EFFECTS;

void startTransition(uint8_t newEffect);

// Initialize strip mapping for circular/radial effects
void initializeStripMapping() {
    // Use pre-calculated LUTs for angles and radii
    for (uint16_t i = 0; i < NUM_LEDS; i++) {
        angles[i] = FastLEDLUTs::get_matrix_angle(i);
        radii[i] = FastLEDLUTs::get_matrix_radius(i);
    }
}

#if FEATURE_ENCODERS
static const uint8_t ENCODER_BASE_COLORS[7][3] = {
    {48, 48, 0},   // Encoder 1 - brightness
    {48, 0, 48},   // Encoder 2 - palette
    {0, 48, 48},   // Encoder 3 - speed
    {48, 16, 0},   // Encoder 4 - fade
    {0, 48, 16},   // Encoder 5 - transition duration
    {16, 0, 48},   // Encoder 6 - reserved
    {32, 32, 32}   // Encoder 7 - reserved
};

static void restoreEncoderLed(uint8_t index) {
    if (!encoderConnected || index >= 8) return;
    if (index == 0) {
        encoder.writeRGB(0, paletteMode ? 56 : 0, 0, 64);
        return;
    }
    const uint8_t* base = ENCODER_BASE_COLORS[index - 1];
    encoder.writeRGB(index, base[0], base[1], base[2]);
}

static void flashEncoderLed(uint8_t index, uint8_t r, uint8_t g, uint8_t b, uint16_t durationMs) {
    if (!encoderConnected || index >= 8) return;
    encoder.writeRGB(index, r, g, b);
    encoderLedFlashUntil[index] = millis() + durationMs;
}

static void setEncoderBaseline() {
    if (!encoderConnected) return;
    for (uint8_t i = 0; i < 8; ++i) {
        encoderLedFlashUntil[i] = 0;
        restoreEncoderLed(i);
    }
    // Center LED used as heartbeat
    encoder.writeRGB(8, 8, 8, 12);
}

static void initializeEncoderHardware() {
    Serial.println("Initializing M5Stack 8Encoder (0x41)...");
    Wire.begin(HardwareConfig::I2C_SDA, HardwareConfig::I2C_SCL);
    Wire.setClock(HardwareConfig::I2C_FREQUENCY);

    encoderConnected = encoder.begin() && encoder.isConnected();
    if (encoderConnected) {
        encoder.resetAll();
        setEncoderBaseline();
        Serial.println("M5Stack 8Encoder connected");
    } else {
        Serial.println("M5Stack 8Encoder not detected - will retry in loop");
        lastEncoderReconnectAttempt = millis();
    }
}

static void handleEncoderInput() {
    uint32_t now = millis();

    if (!encoderConnected) {
        if (now - lastEncoderReconnectAttempt >= 2000) {
            lastEncoderReconnectAttempt = now;
            if (encoder.begin() && encoder.isConnected()) {
                encoderConnected = true;
                encoder.resetAll();
                setEncoderBaseline();
                Serial.println("M5Stack 8Encoder reconnected");
            }
        }
        return;
    }

    if (!encoder.isConnected()) {
        encoderConnected = false;
        lastEncoderReconnectAttempt = now;
        Serial.println("M5Stack 8Encoder connection lost");
        return;
    }

    for (uint8_t channel = 0; channel < 8; ++channel) {
        int32_t rawDelta = encoder.getRelCounter(channel);
        if (rawDelta != 0) {
            encoder.resetCounter(channel);

            int32_t steps = rawDelta / 2;
            if (steps == 0) steps = (rawDelta > 0) ? 1 : -1;
            int8_t direction = (steps > 0) ? 1 : -1;
            uint8_t iterations = static_cast<uint8_t>(abs(steps));
            if (iterations == 0) iterations = 1;

            switch (channel) {
                case 0: { // Effect selection
                    for (uint8_t i = 0; i < iterations; ++i) {
                        uint8_t next = (direction > 0)
                            ? static_cast<uint8_t>((currentEffect + 1) % NUM_EFFECTS)
                            : static_cast<uint8_t>(currentEffect == 0 ? NUM_EFFECTS - 1 : currentEffect - 1);
                        startTransition(next);
                    }
                    Serial.printf("Encoder 0 -> effect %s\n", effects[currentEffect].name);
                    flashEncoderLed(0, 0, 96, 160, 200);
                    break;
                }
                case 1: { // Brightness
                    int32_t newBrightness = static_cast<int32_t>(currentBrightness) + direction * BRIGHTNESS_STEP * iterations;
                    if (newBrightness < MIN_BRIGHTNESS) newBrightness = MIN_BRIGHTNESS;
                    if (newBrightness > MAX_BRIGHTNESS) newBrightness = MAX_BRIGHTNESS;
                    currentBrightness = static_cast<uint8_t>(newBrightness);
                    FastLED.setBrightness(currentBrightness);
                    Serial.printf("Encoder 1 -> brightness %u\n", currentBrightness);
                    flashEncoderLed(1, 120, 120, 0, 200);
                    break;
                }
                case 2: { // Palette selection
                    if (totalPaletteCount == 0) {
                        totalPaletteCount = static_cast<uint16_t>(gGradientPaletteCount + gCrameriPaletteCount);
                    }
                    if (totalPaletteCount == 0) break;
                    int32_t newIndex = static_cast<int32_t>(currentPaletteIndex) + direction * iterations;
                    while (newIndex < 0) newIndex += totalPaletteCount;
                    currentPaletteIndex = static_cast<uint16_t>(newIndex % totalPaletteCount);
                    if (currentPaletteIndex < gGradientPaletteCount) {
                        targetPalette = CRGBPalette16(gGradientPalettes[currentPaletteIndex]);
                    } else {
                        uint16_t crIdx = currentPaletteIndex - gGradientPaletteCount;
                        targetPalette = CRGBPalette16(gCrameriPalettes[crIdx]);
                    }
                    if (ENABLE_LINEARISE_PALETTES) linearisePalette(targetPalette);
                    if (ENABLE_PALETTE_CURATION) curatePalette(targetPalette);
                    lastPaletteChangeMs = now;
                    Serial.printf("Encoder 2 -> palette index %u\n", currentPaletteIndex);
                    flashEncoderLed(2, 160, 0, 160, 200);
                    break;
                }
                case 3: { // Palette speed
                    int32_t newSpeed = static_cast<int32_t>(paletteSpeed) + direction * iterations;
                    if (newSpeed < 1) newSpeed = 1;
                    if (newSpeed > 50) newSpeed = 50;
                    paletteSpeed = static_cast<uint8_t>(newSpeed);
                    Serial.printf("Encoder 3 -> speed %u\n", paletteSpeed);
                    flashEncoderLed(3, 0, 140, 140, 200);
                    break;
                }
                case 4: { // Fade amount
                    int32_t newFade = static_cast<int32_t>(fadeAmount) + direction * iterations;
                    if (newFade < 1) newFade = 1;
                    if (newFade > 80) newFade = 80;
                    fadeAmount = static_cast<uint8_t>(newFade);
                    Serial.printf("Encoder 4 -> fade %u\n", fadeAmount);
                    flashEncoderLed(4, 140, 64, 0, 200);
                    break;
                }
                case 5: { // Transition duration
                    int32_t newDuration = static_cast<int32_t>(transitionDuration) + direction * 100 * iterations;
                    if (newDuration < 200) newDuration = 200;
                    if (newDuration > 6000) newDuration = 6000;
                    transitionDuration = static_cast<uint32_t>(newDuration);
                    Serial.printf("Encoder 5 -> transition %ums\n", transitionDuration);
                    flashEncoderLed(5, 0, 140, 80, 200);
                    break;
                }
                default:
                    Serial.printf("Encoder %u not mapped yet\n", channel);
                    flashEncoderLed(channel, 120, 0, 0, 150);
                    break;
            }
        }
    }

    for (uint8_t channel = 0; channel < 8; ++channel) {
        if (encoderLedFlashUntil[channel] != 0 && now >= encoderLedFlashUntil[channel]) {
            encoderLedFlashUntil[channel] = 0;
            restoreEncoderLed(channel);
        }
    }

    for (uint8_t channel = 0; channel < 8; ++channel) {
        bool pressed = encoder.getKeyPressed(channel);
        if (pressed != encoderButtonState[channel]) {
            encoderButtonState[channel] = pressed;

            if (!pressed) {
                switch (channel) {
                    case 0:
                        paletteMode = !paletteMode;
                        paletteModeChanged = true;
                        Serial.printf("Encoder 0 button -> palette mode %s\n", paletteMode ? "ON" : "OFF");
                        restoreEncoderLed(0);
                        flashEncoderLed(0, paletteMode ? 120 : 0, 0, 160, 250);
                        break;
                    case 1:
                        currentBrightness = HardwareConfig::DEFAULT_BRIGHTNESS;
                        FastLED.setBrightness(currentBrightness);
                        Serial.println("Encoder 1 button -> brightness reset");
                        flashEncoderLed(1, 150, 150, 0, 200);
                        break;
                    default:
                        break;
                }
            } else {
                flashEncoderLed(channel, 180, 180, 180, 120);
            }
        }
    }
}
#endif

// ============== BASIC EFFECTS ==============

// Rainbow functions removed - NO RAINBOWS!

void confetti() {
    fadeToBlackBy(leds, NUM_LEDS, 10);
    int pos = random16(NUM_LEDS);
    leds[pos] += CHSV(gHue + random8(64), 200, 255);
}

void sinelon() {
    fadeToBlackBy(leds, NUM_LEDS, 20);
    int pos = beatsin16(13, 0, NUM_LEDS-1);
    leds[pos] += CHSV(gHue, 255, 192);
}

void juggle() {
    fadeToBlackBy(leds, NUM_LEDS, 20);
    uint8_t dothue = 0;
    for(int i = 0; i < 8; i++) {
        leds[beatsin16(i+7, 0, NUM_LEDS-1)] |= CHSV(dothue, 200, 255);
        dothue += 32;
    }
}

void bpm() {
    uint8_t BeatsPerMinute = 62;
    uint8_t beat = beatsin8(BeatsPerMinute, 64, 255);
    for(int i = 0; i < NUM_LEDS; i++) {
        leds[i] = ColorFromPalette(currentPalette, gHue+(i*2), beat-gHue+(i*10), LINEARBLEND);
    }
}

// ============== ADVANCED WAVE EFFECTS ==============

void waveEffect() {
    static uint16_t wavePosition = 0;
    
    fadeToBlackBy(leds, NUM_LEDS, fadeAmount);
    
    // Use beatsin16 for automatic wave position
    wavePosition = beatsin16(paletteSpeed, 0, 65535);
    
    for (uint16_t i = 0; i < NUM_LEDS; i++) {
        // Multiple wave layers using triwave8 and quadwave8
        uint8_t wave1 = triwave8((i * 10 + (wavePosition >> 8)) & 0xFF);
        uint8_t wave2 = quadwave8((i * 7 - (wavePosition >> 9)) & 0xFF);
        
        // Combine waves
        uint8_t brightness = scale8(wave1, wave2);
        uint8_t colorIndex = angles[i] + (wavePosition >> 8);
        
        CRGB color = ColorFromPalette(currentPalette, colorIndex, brightness, LINEARBLEND);
        leds[i] = color;
    }
}

void rippleEffect() {
    static struct {
        uint16_t center;  // Fixed-point 8.8 format
        uint16_t radius;  // Fixed-point 8.8 format
        uint8_t speed;    // Speed in fixed units
        uint8_t hue;
        bool active;
    } ripples[5];
    
    fadeToBlackBy(leds, NUM_LEDS, fadeAmount);
    
    // Spawn new ripples
    if (random8() < 20) {
        for (uint8_t i = 0; i < 5; i++) {
            if (!ripples[i].active) {
                ripples[i].center = random16(NUM_LEDS) << 8;  // Convert to 8.8 fixed-point
                ripples[i].radius = 0;
                ripples[i].speed = 128 + random8()/2;  // Speed range: 128-191 (0.5-0.75 in fixed)
                ripples[i].hue = random8();
                ripples[i].active = true;
                break;
            }
        }
    }
    
    // Update and render ripples
    for (uint8_t r = 0; r < 5; r++) {
        if (!ripples[r].active) continue;
        
        // Update radius with speed scaled by paletteSpeed
        uint16_t speedScaled = ((uint16_t)ripples[r].speed * paletteSpeed) / 10;
        ripples[r].radius += speedScaled;
        
        if (ripples[r].radius > (NUM_LEDS << 8)) {  // Compare in fixed-point
            ripples[r].active = false;
            continue;
        }
        
        for (uint16_t i = 0; i < NUM_LEDS; i++) {
            // Use pre-calculated distance from LUT
            uint8_t distance = FastLEDLUTs::get_matrix_radius(i);
            int16_t wavePos = distance - (ripples[r].radius >> 8);
            
            if (abs(wavePos) < 10) {
                // Use gaussian LUT for smooth falloff
                uint8_t gaussDist = abs(wavePos) * 12;  // Scale to fit gaussian LUT range
                uint8_t waveBright = FastLEDLUTs::gaussian_lut_read(gaussDist);
                
                // Scale brightness based on ripple age
                uint8_t ageFade = 255 - scale8(ripples[r].radius >> 8, 255);
                uint8_t brightness = scale8(waveBright, ageFade);
                
        CRGB color = ColorFromPalette(currentPalette, ripples[r].hue + distance, brightness, LINEARBLEND);
                leds[i] += color;
            }
        }
    }
}

void interferenceEffect() {
    static uint16_t wave1Phase = 0;
    static uint16_t wave2Phase = 0;
    
    fadeToBlackBy(leds, NUM_LEDS, fadeAmount);
    
    // Use fixed-point phase increments
    wave1Phase += scale16by8(655, paletteSpeed);  // ~paletteSpeed/20 in 16-bit space
    wave2Phase -= scale16by8(437, paletteSpeed);  // ~paletteSpeed/30 in 16-bit space
    
    for (uint16_t i = 0; i < NUM_LEDS; i++) {
        // Calculate position in 16-bit space
        uint16_t pos16 = (i * 65535U) / NUM_LEDS;
        
        // Use sin16 for wave calculations
        // sin16 returns -32767 to 32767, we need 0-255
        int16_t wave1_raw = sin16(pos16 * 4 + wave1Phase);
        int16_t wave2_raw = sin16(pos16 * 6 + wave2Phase);
        
        // Convert to 0-255 range
        uint8_t wave1 = (wave1_raw + 32768) >> 8;
        uint8_t wave2 = (wave2_raw + 32768) >> 8;
        
        // Average the waves
        uint8_t brightness = (wave1 + wave2) >> 1;
        
        uint8_t hue = (wave1Phase >> 8) + angles[i];
        
        CRGB color = ColorFromPalette(currentPalette, hue, brightness, LINEARBLEND);
        leds[i] = color;
    }
}

// ============== MATHEMATICAL PATTERNS ==============

void fibonacciSpiral() {
    static uint16_t spiralPhase = 0;
    
    fadeToBlackBy(leds, NUM_LEDS, fadeAmount);
    // Use scale16by8 for precise speed control
    spiralPhase += scale16by8(327, paletteSpeed);  // ~paletteSpeed/50 in 16-bit
    
    // Fibonacci sequence positions
    const uint8_t fib[] = {1, 1, 2, 3, 5, 8, 13, 21, 34, 55};
    
    for (int f = 0; f < 10; f++) {
        int pos = (fib[f] + (spiralPhase >> 8)) % NUM_LEDS;
        uint8_t hue = f * 25 + gHue;
        uint8_t brightness = qadd8(55, 200 - (f * 20));  // Ensure minimum brightness
        
        // Draw with falloff using qadd8 for safe addition
        for (int i = -3; i <= 3; i++) {
            int ledPos = (pos + i + NUM_LEDS) % NUM_LEDS;
            uint8_t distance = abs(i);
            uint8_t fadeBrightness = qsub8(brightness, distance * 60);
            if (fadeBrightness > 0) {
                leds[ledPos] += CHSV(hue, 255, fadeBrightness);
            }
        }
    }
}

void kaleidoscope() {
    static uint16_t offset = 0;
    offset += paletteSpeed;
    
    // Create symmetrical patterns
    for (int i = 0; i < NUM_LEDS / 2; i++) {
        uint8_t hue = FastLEDLUTs::sin_lut_read((i * 10 + offset) & 0xFF) + gHue;
        uint8_t brightness = FastLEDLUTs::sin_lut_read((i * 15 + offset * 2) & 0xFF);
        
        CRGB color = CHSV(hue, 255, brightness);
        
        leds[i] = color;
        leds[NUM_LEDS - 1 - i] = color;  // Mirror
    }
}

void plasma() {
    static uint16_t time = 0;
    time += paletteSpeed;
    
    for (int i = 0; i < NUM_LEDS; i++) {
        // Use beatsin8 for automatic wave generation
        // beatsin8(bpm, min, max, offset)
        uint8_t v1 = beatsin8(62, 0, 255, time, i * 8);
        uint8_t v2 = beatsin8(41, 0, 255, time + 1000, i * 5);
        uint8_t v3 = beatsin8(31, 0, 255, time + 2000, i * 3);
        
        // Combine waves for hue - use scale8 for averaging
        uint16_t hueSum = v1 + v2 + v3;
        uint8_t hue = scale8(hueSum / 3, 170) + 43 + gHue;  // ~42.5 * 4 = 170
        
        // Brightness from two waves
        uint8_t brightness = scale8((v1 + v2) >> 1, 127) + 128;  // ~63.75 * 2 = 127
        
        leds[i] = CHSV(hue, 255, brightness);
    }
}

// ============== NEW VISUALISATIONS ==============

// 1) Noise Flow Field (organic motion using Perlin noise)
void noiseFlowFieldEffect() {
    static uint16_t z = 0;
    z += paletteSpeed * 2;
    for (uint8_t y = 0; y < 9; y++) {
        for (uint8_t x = 0; x < 9; x++) {
            uint8_t idx = y * 9 + x;
            uint8_t n1 = inoise8(x * 24, y * 24, z);
            uint8_t n2 = inoise8(255 - x * 24, 255 - y * 24, z);
            uint8_t ang = n1;
            uint8_t mag = qadd8(n2, 40);
            uint8_t bri = scale8(mag, 200);
            leds[idx] = ColorFromPalette(currentPalette, ang, bri, LINEARBLEND);
        }
    }
}

// 2) Voronoi Cells (moving seeds, soft edges)
void voronoiCellsEffect() {
    struct Seed { uint8_t x; uint8_t y; uint8_t hue; uint16_t t; };
    static Seed seeds[4];
    static bool init = false;
    if (!init) {
        for (uint8_t i = 0; i < 4; ++i) {
            seeds[i].x = random8(1, 8);
            seeds[i].y = random8(1, 8);
            seeds[i].hue = random8();
            seeds[i].t = random16();
        }
        init = true;
    }
    for (uint8_t i = 0; i < 4; ++i) {
        seeds[i].t += paletteSpeed;
        seeds[i].x = map8(sin8(seeds[i].t >> 2), 1, 7);
        seeds[i].y = map8(cos8(seeds[i].t >> 3), 1, 7);
        seeds[i].hue += 1;
    }
    for (uint8_t y = 0; y < 9; y++) {
        for (uint8_t x = 0; x < 9; x++) {
            uint8_t idx = y * 9 + x;
            uint16_t bestD = 0xFFFF; uint8_t owner = 0;
            for (uint8_t i = 0; i < 4; ++i) {
                int8_t dx = (int8_t)x - (int8_t)seeds[i].x;
                int8_t dy = (int8_t)y - (int8_t)seeds[i].y;
                uint16_t d = dx * dx + dy * dy;
                if (d < bestD) { bestD = d; owner = i; }
            }
            uint8_t dist = bestD > 255 ? 255 : bestD;
            uint8_t bri = 255 - (dist << 3);
            uint8_t pi = seeds[owner].hue + (dist << 2);
            leds[idx] = ColorFromPalette(currentPalette, pi, bri, LINEARBLEND);
        }
    }
    blur1d(leds, NUM_LEDS, 10);
}

// 3) Spiral Tunnel (polar mapping using precomputed angles/radii)
void spiralTunnelEffect() {
    static uint16_t t = 0; t += paletteSpeed * 3;
    for (uint8_t i = 0; i < NUM_LEDS; ++i) {
        uint8_t r = radii[i];
        uint8_t a = angles[i];
        uint8_t depth = 255 - r;
        uint8_t idx = a + (t >> 3) + (depth >> 1);
        uint8_t bri = qadd8(depth, 40);
        leds[i] = ColorFromPalette(currentPalette, idx, bri, LINEARBLEND);
    }
}

// 4) Palette Twinkle (low-density sparkles using current palette)
void paletteTwinkleEffect() {
    fadeToBlackBy(leds, NUM_LEDS, 20);
    uint8_t sparks = 4;
    for (uint8_t s = 0; s < sparks; ++s) {
        uint8_t i = random8(NUM_LEDS);
        uint8_t pi = random8();
        uint8_t bri = random8(120, 255);
        leds[i] += ColorFromPalette(currentPalette, pi, bri, LINEARBLEND);
    }
}
// ============== NATURE-INSPIRED EFFECTS ==============

void fire() {
    static byte heat[NUM_LEDS];
    
    // Cool down every cell a little
    for(int i = 0; i < NUM_LEDS; i++) {
        heat[i] = qsub8(heat[i], random8(0, ((55 * 10) / NUM_LEDS) + 2));
    }
    
    // Heat from each cell drifts up and diffuses
    for(int k = NUM_LEDS - 1; k >= 2; k--) {
        heat[k] = (heat[k - 1] + heat[k - 2] + heat[k - 2]) / 3;
    }
    
    // Randomly ignite new sparks
    if(random8() < 120) {
        int y = random8(7);
        heat[y] = qadd8(heat[y], random8(160, 255));
    }
    
    // Map heat to LED colors
    for(int j = 0; j < NUM_LEDS; j++) {
        CRGB color = HeatColor(heat[j]);
        leds[j] = color;
    }
}

void ocean() {
    static uint16_t waterOffset = 0;
    waterOffset += paletteSpeed / 2;
    
    for (int i = 0; i < NUM_LEDS; i++) {
        // Create wave-like motion using LUTs
        uint8_t wave1 = FastLEDLUTs::sin_lut_read(((i * 10) + waterOffset) & 0xFF);
        uint8_t wave2 = FastLEDLUTs::sin_lut_read(((i * 7) - waterOffset * 2) & 0xFF);
        uint8_t combinedWave = (wave1 + wave2) / 2;
        
        // Ocean colors from deep blue to cyan
        uint8_t hue = 160 + (combinedWave >> 3);  // Blue range
        uint8_t brightness = 100 + (combinedWave >> 1);
        uint8_t saturation = 255 - (combinedWave >> 2);
        
        leds[i] = CHSV(hue, saturation, brightness);
    }
}

// ============== NEW MATRIX EFFECTS ==============

void liquidMetalEffect() {
    // Enhanced lava lamp with smooth edges for light pipes
    static struct Blob {
        uint16_t x, y;         // Position in 8.8 fixed-point
        uint16_t radius;       // Size in 8.8 fixed-point
        int16_t vx, vy;        // Velocity
        uint8_t temperature;   // Heat level
        uint16_t phase;        // Individual phase for pulsing
    } blobs[3];  // Fewer, larger blobs
    
    static bool initialized = false;
    static uint16_t globalPhase = 0;
    
    if (!initialized) {
        for (uint8_t i = 0; i < 3; i++) {
            blobs[i].x = random16(2 * 256, 7 * 256);
            blobs[i].y = random16(2 * 256, 7 * 256);
            blobs[i].radius = random16(768, 1024);  // Larger blobs
            blobs[i].vx = random16(32) - 16;        // Very slow movement
            blobs[i].vy = random16(32) - 16;
            blobs[i].temperature = random8(180, 255);
            blobs[i].phase = random16();
        }
        initialized = true;
    }
    
    globalPhase += paletteSpeed * 5;
    
    // Very slow, smooth blob movement
    for (uint8_t b = 0; b < 3; b++) {
        // Smooth sinusoidal velocity modulation
        int16_t vxMod = sin8((globalPhase >> 8) + blobs[b].phase) - 128;
        int16_t vyMod = sin8((globalPhase >> 7) + blobs[b].phase + 64) - 128;
        
        blobs[b].x += (blobs[b].vx + (vxMod >> 4)) >> 1;
        blobs[b].y += (blobs[b].vy + (vyMod >> 4)) >> 1;
        
        // Soft boundaries with smooth reflection
        if (blobs[b].x < 512 || blobs[b].x > 7 * 256) {
            blobs[b].vx = -blobs[b].vx * 3 / 4;  // Energy loss for realism
            blobs[b].x = constrain(blobs[b].x, 512, 7 * 256);
        }
        if (blobs[b].y < 512 || blobs[b].y > 7 * 256) {
            blobs[b].vy = -blobs[b].vy * 3 / 4;
            blobs[b].y = constrain(blobs[b].y, 512, 7 * 256);
        }
        
        // Gentle radius pulsing
        blobs[b].phase += blobs[b].temperature;
        uint8_t pulseMod = sin8(blobs[b].phase >> 8);
        blobs[b].radius = 768 + (pulseMod >> 2);
        
        // Very gradual velocity changes
        if (random8() < 5) {
            blobs[b].vx += random8(8) - 4;
            blobs[b].vy += random8(8) - 4;
            blobs[b].vx = constrain(blobs[b].vx, -24, 24);
            blobs[b].vy = constrain(blobs[b].vy, -24, 24);
        }
    }
    
    // Slow fade for trail effect
    fadeToBlackBy(leds, NUM_LEDS, 8);
    
    // Render with heavy gaussian smoothing
    for (uint8_t y = 0; y < 9; y++) {
        for (uint8_t x = 0; x < 9; x++) {
            uint8_t idx = y * 9 + x;
            uint32_t totalField = 0;
            uint32_t totalTemp = 0;
            
            for (uint8_t b = 0; b < 3; b++) {
                int16_t dx = (x << 8) - blobs[b].x;
                int16_t dy = (y << 8) - blobs[b].y;
                uint16_t dist = sqrt16((dx * dx + dy * dy) >> 8);
                
                // Use gaussian LUT for ultra-smooth edges
                if (dist < 255) {
                    uint8_t influence = FastLEDLUTs::gaussian_lut_read(dist >> 1);
                    
                    // Temperature-weighted field
                    uint16_t field = scale8(influence, influence);  // Square for sharper center
                    totalField += field;
                    totalTemp += field * blobs[b].temperature;
                }
            }
            
            if (totalField > 0) {
                // Normalize temperature safely
                uint8_t avgTemp = 0;
                if (totalField >= 256) {
                    avgTemp = totalTemp / (totalField >> 8);
                } else {
                    avgTemp = (totalTemp * 256) / totalField;
                }
                
                // Map to palette with smooth brightness curve
                uint8_t brightness = (totalField >> 2) > 255 ? 255 : (totalField >> 2);
                brightness = scale8(brightness, brightness);  // Gamma correction
                
                // Smooth color transitions
        CRGB newColor = ColorFromPalette(currentPalette, avgTemp, brightness, LINEARBLEND);
                leds[idx] = blend(leds[idx], newColor, 200);
            }
        }
    }
}

void gravityFlowEffect() {
    // Smooth gravity flow - no particles, just continuous fields
    static uint16_t flowPhase = 0;
    static struct Source {
        uint8_t x, y;          // Grid position
        int8_t strength;       // Field strength
        uint8_t hue;           // Base color
        uint16_t phase;        // Individual phase offset
    } sources[3] = {
        {2, 2, 100, 160, 0},      // Blue field
        {6, 6, 80, 96, 21845},    // Purple field
        {4, 7, 90, 200, 43690}    // Cyan field
    };
    
    // Slow phase progression for smooth flow
    flowPhase += paletteSpeed * 10;
    
    // Clear with very slow fade for persistence
    fadeToBlackBy(leds, NUM_LEDS, 5);
    
    // Calculate field influence at each LED
    for (uint8_t y = 0; y < 9; y++) {
        for (uint8_t x = 0; x < 9; x++) {
            uint8_t idx = y * 9 + x;
            uint16_t totalField = 0;
            uint16_t totalHue = 0;
            
            // Sum influence from all sources
            for (uint8_t s = 0; s < 3; s++) {
                // Distance calculation
                int8_t dx = x - sources[s].x;
                int8_t dy = y - sources[s].y;
                uint8_t dist = sqrt16(dx * dx + dy * dy);
                
                if (dist < 6) {  // 6 grid units influence radius
                    // Smooth gaussian falloff
                    uint8_t influence = FastLEDLUTs::gaussian_lut_read(dist * 21);
                    
                    // Add wave modulation for flow effect
                    uint16_t waveAngle = flowPhase + sources[s].phase + (dist << 12);
                    uint8_t waveMod = sin8(waveAngle >> 8);
                    
                    // Scale influence by source strength and wave
                    influence = scale8(influence, sources[s].strength);
                    influence = scale8(influence, 128 + (waveMod >> 1));
                    
                    totalField += influence;
                    totalHue += sources[s].hue * influence;
                }
            }
            
            if (totalField > 0) {
                // Normalize hue
                uint8_t finalHue = totalHue / max(1, totalField >> 8);
                
                // Limit brightness for smooth appearance
                uint8_t brightness = min(200, totalField >> 1);
                
                // Blend with existing color for smooth transitions
                CRGB newColor = CHSV(finalHue, 255, brightness);
                leds[idx] = blend(leds[idx], newColor, 128);
            }
        }
    }
    
    // Add slow-moving gravity wells with large influence
    for (uint8_t s = 0; s < 3; s++) {
        // Slowly orbit the sources
        sources[s].phase += sources[s].strength;
        
        // Draw smooth source cores (3x3 minimum)
        for (int8_t dy = -1; dy <= 1; dy++) {
            for (int8_t dx = -1; dx <= 1; dx++) {
                uint8_t x = sources[s].x + dx;
                uint8_t y = sources[s].y + dy;
                if (x < 9 && y < 9) {
                    uint8_t idx = y * 9 + x;
                    uint8_t dist = abs(dx) + abs(dy);
                    uint8_t coreBright = beatsin8(20 + s * 7, 140, 255, 0, sources[s].phase >> 8);
                    coreBright = scale8(coreBright, 255 - dist * 60);
                    
                    CRGB coreColor = CHSV(sources[s].hue, 255, coreBright);
                    leds[idx] = blend(leds[idx], coreColor, 200);
                }
            }
        }
    }
}

void breathingOrbsEffect() {
    // Slowly breathing orbs - perfect for light pipes
    static struct Orb {
        uint8_t x, y;          // Grid position
        uint8_t hue;           // Base color
        uint16_t phase;        // Breathing phase
        uint8_t size;          // Base size
        uint8_t speed;         // Breathing speed
    } orbs[4] = {
        {2, 2, 0, 0, 3, 40},      // Red orb
        {6, 2, 96, 16384, 4, 35}, // Purple orb
        {2, 6, 160, 32768, 3, 45}, // Blue orb
        {6, 6, 64, 49152, 4, 30}  // Green orb
    };
    
    // Very slow fade for smooth trails
    fadeToBlackBy(leds, NUM_LEDS, 3);
    
    // Update and render each orb
    for (uint8_t o = 0; o < 4; o++) {
        // Update breathing phase
        orbs[o].phase += orbs[o].speed * paletteSpeed / 4;
        
        // Calculate current size with smooth sine breathing
        uint8_t breathe = sin8(orbs[o].phase >> 8);
        uint8_t currentSize = orbs[o].size + (breathe >> 4);
        
        // Render orb with gaussian falloff
        for (uint8_t y = 0; y < 9; y++) {
            for (uint8_t x = 0; x < 9; x++) {
                // Calculate distance from orb center
                int8_t dx = x - orbs[o].x;
                int8_t dy = y - orbs[o].y;
                uint8_t dist = sqrt16(dx * dx + dy * dy);
                
                if (dist < currentSize * 32) {
                    uint8_t idx = y * 9 + x;
                    
                    // Smooth gaussian falloff
                    uint8_t influence = FastLEDLUTs::gaussian_lut_read(dist * 4);
                    
                    // Brightness modulated by breathing
                    uint8_t brightness = scale8(influence, breathe);
                    brightness = scale8(brightness, 200);  // Max brightness limit
                    
                    // Color with slight hue variation
                    uint8_t hue = orbs[o].hue + (dist << 1);
                    
                    // High saturation in center, lower at edges
                    uint8_t saturation = 255 - (dist << 2);
                    
                    CRGB newColor = CHSV(hue, saturation, brightness);
                    
                    // Additive blending for overlap effects
                    leds[idx] += scale8(newColor.r, 128);
                    leds[idx] += scale8(newColor.g, 128);
                    leds[idx] += scale8(newColor.b, 128);
                }
            }
        }
        
        // Very slow position drift
        if (random8() < 2) {
            orbs[o].phase += random16(1000);
        }
    }
}

void auroraWavesEffect() {
    // Slow moving aurora curtains - leverages light pipe blending
    static uint16_t phase1 = 0;
    static uint16_t phase2 = 21845;
    static uint16_t phase3 = 43690;
    
    // Very slow phase progression
    phase1 += paletteSpeed * 3;
    phase2 += paletteSpeed * 2;
    phase3 += paletteSpeed * 4;
    
    // Minimal fade for persistence
    fadeToBlackBy(leds, NUM_LEDS, 2);
    
    for (uint8_t y = 0; y < 9; y++) {
        for (uint8_t x = 0; x < 9; x++) {
            uint8_t idx = y * 9 + x;
            
            // Three overlapping curtains
            uint8_t curtain1 = sin8((x * 28) + (phase1 >> 8) + (y * 10));
            uint8_t curtain2 = sin8((x * 37) + (phase2 >> 8) - (y * 13));
            uint8_t curtain3 = sin8((x * 23) + (phase3 >> 8) + (y * 17));
            
            // Combine curtains with different weights
            uint16_t combined = (curtain1 >> 1) + (curtain2 >> 2) + (curtain3 >> 2);
            
            // Vertical gradient
            uint8_t vGradient = 255 - (y * 28);
            combined = scale8(combined, vGradient);
            
            // Map to aurora colors (greens, blues, purples)
            uint8_t hue = 96 + (combined >> 2);  // Blue-green range
            uint8_t saturation = 200 + (combined >> 3);
            uint8_t brightness = combined >> 1;
            
            // Add shimmer
            if (random8() < 3) {
                brightness = qadd8(brightness, 100);
            }
            
            CRGB newColor = CHSV(hue, saturation, brightness);
            leds[idx] = blend(leds[idx], newColor, 180);
        }
    }
}

// ============== TRANSITION SYSTEM ==============

void startTransition(uint8_t newEffect) {
    if (newEffect == currentEffect) return;
    
    // Save current LED state
    for (int i = 0; i < NUM_LEDS; i++) {
        transitionBuffer[i] = leds[i];
    }
    
    previousEffect = currentEffect;
    currentEffect = newEffect;
    inTransition = true;
    transitionStart = millis();
    transitionProgress = 0;
}

void updateTransition() {
    if (!inTransition) return;
    
    uint32_t elapsed = millis() - transitionStart;
    
    if (elapsed >= transitionDuration) {
        transitionProgress = 255;
        inTransition = false;
        return;
    }
    
    // Calculate progress in 0-255 range
    transitionProgress = (elapsed * 255) / transitionDuration;
    
    // Smooth easing function using FastLED's ease8InOutCubic
    uint8_t eased = ease8InOutCubic(transitionProgress);
    
    // Blend between old and new effect
    for (int i = 0; i < NUM_LEDS; i++) {
        CRGB oldColor = transitionBuffer[i];
        CRGB newColor = leds[i];
        
        leds[i] = blend(oldColor, newColor, eased);
    }
}

// Array of effects
Effect effects[] = {
    // Basic effects (NO RAINBOWS!)
    {"Confetti", confetti},
    {"Sinelon", sinelon},
    {"Juggle", juggle},
    {"BPM", bpm},
    
    // Wave effects
    {"Wave", waveEffect},
    {"Ripple", rippleEffect},
    {"Interference", interferenceEffect},
    
    // Mathematical patterns
    {"Fibonacci", fibonacciSpiral},
    {"Kaleidoscope", kaleidoscope},
    {"Plasma", plasma},
    
    // Nature effects
    {"Fire", fire},
    {"Ocean", ocean},
    
    // New smooth effects for light pipes
    {"Liquid Metal", liquidMetalEffect},
    {"Gravity Flow", gravityFlowEffect},
    {"Breathing Orbs", breathingOrbsEffect},
    {"Aurora Waves", auroraWavesEffect},
    {"Noise Flow Field", noiseFlowFieldEffect},
    {"Voronoi Cells", voronoiCellsEffect},
    {"Spiral Tunnel", spiralTunnelEffect},
    {"Palette Twinkle", paletteTwinkleEffect}
};

const uint8_t NUM_EFFECTS = sizeof(effects) / sizeof(effects[0]);

void setup() {
    // Initialize serial
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n=== Light Crystals Enhanced ===");
    Serial.println("Board: ESP32-S3");
    Serial.print("LED Pin: GPIO");
    Serial.println(DATA_PIN);
    Serial.print("LEDs: ");
    Serial.println(NUM_LEDS);
    Serial.print("Effects: ");
    Serial.println(NUM_EFFECTS);

    // Initialize power pin
    pinMode(POWER_PIN_GPIO, OUTPUT);
    digitalWrite(POWER_PIN_GPIO, HIGH);

    // Initialize button
    pinMode(BUTTON_PIN_GPIO, INPUT_PULLUP);

    // Initialize LEDs
    FastLED.addLeds<LED_TYPE, DATA_PIN, COLOR_ORDER>(leds, NUM_LEDS);
    FastLED.setBrightness(currentBrightness);
    FastLED.setCorrection(TypicalLEDStrip);
    FastLED.setDither(1);  // Enable temporal dithering
    FastLED.setMaxRefreshRate(0, true);  // Enable async rendering
    FastLED.setMaxPowerInVoltsAndMilliamps(5, MAX_CURRENT_MA);

    // Prepare lookup tables that depend on runtime math
    initSRGB2LinearLUT();

    // Initialize strip mapping
    initializeStripMapping();

#if FEATURE_ENCODERS
    if (HardwareConfig::HAS_ENCODER) {
        initializeEncoderHardware();
    }
#endif
    
    // Initialize palette (combined Standard + Crameri)
    totalPaletteCount = (uint16_t)gGradientPaletteCount + (uint16_t)gCrameriPaletteCount;
    currentPaletteIndex = 0;
    if (currentPaletteIndex < gGradientPaletteCount) {
        currentPalette = CRGBPalette16(gGradientPalettes[currentPaletteIndex]);
    } else {
        uint16_t crIdx = currentPaletteIndex - gGradientPaletteCount;
        currentPalette = CRGBPalette16(gCrameriPalettes[crIdx]);
    }
    if (ENABLE_LINEARISE_PALETTES) linearisePalette(currentPalette);
    if (ENABLE_PALETTE_CURATION) curatePalette(currentPalette);
    targetPalette = currentPalette;
    
    // Clear LEDs
    FastLED.clear(true);
    
    Serial.println("=== Setup Complete ===\n");
}

void handleButton() {
    if (digitalRead(BUTTON_PIN_GPIO) == LOW) {
        if (millis() - lastButtonPress > 500) {
            lastButtonPress = millis();
            
            // Start transition to next effect
            uint8_t nextEffect = (currentEffect + 1) % NUM_EFFECTS;
            startTransition(nextEffect);
            
            Serial.print("Transitioning to: ");
            Serial.println(effects[nextEffect].name);
        }
    }
}

void updatePalette() {
    // Smoothly blend between palettes
    if (paletteModeChanged) {
        lastPaletteChangeMs = millis();
        paletteModeChanged = false;
    }

    uint32_t dwell = paletteMode ? PALETTE_MODE_DWELL_MS : PALETTE_DWELL_MS;
    if (millis() - lastPaletteChangeMs > dwell) {  // Change palette per dwell
        lastPaletteChangeMs = millis();
        if (totalPaletteCount == 0) {
            totalPaletteCount = (uint16_t)gGradientPaletteCount + (uint16_t)gCrameriPaletteCount;
        }
        if (PALETTE_RANDOMISE && !paletteMode) {
            currentPaletteIndex = random16(totalPaletteCount);
        } else {
            currentPaletteIndex = (currentPaletteIndex + 1) % totalPaletteCount;
        }
        if (currentPaletteIndex < gGradientPaletteCount) {
            targetPalette = CRGBPalette16(gGradientPalettes[currentPaletteIndex]);
        } else {
            uint16_t crIdx = currentPaletteIndex - gGradientPaletteCount;
            targetPalette = CRGBPalette16(gCrameriPalettes[crIdx]);
        }
        if (ENABLE_LINEARISE_PALETTES) linearisePalette(targetPalette);
        if (ENABLE_PALETTE_CURATION) curatePalette(targetPalette);
    }

    // Blend towards target palette
    nblendPaletteTowardPalette(currentPalette, targetPalette, 24);
}

void loop() {
    // Start performance measurement
    metrics.beginFrame();
    
    // Handle button
    handleButton();

#if FEATURE_ENCODERS
    if (HardwareConfig::HAS_ENCODER) {
        handleEncoderInput();
    }
#endif

    // Update palette blending
    updatePalette();
    
    // Measure effect performance
    metrics.beginEffect(currentEffect);
    
    // If not in transition, run current effect
    if (!inTransition) {
        effects[currentEffect].function();
    } else {
        // Run the new effect to generate target colors
        effects[currentEffect].function();
        // Then apply transition blending
        updateTransition();
    }
    
    metrics.endEffect();
    
    // Optional global post-processing
    if (ENABLE_AUTO_EXPOSURE) {
        uint32_t sumLuma = 0;
        for (int i = 0; i < NUM_LEDS; ++i) {
            const CRGB &c = leds[i];
            uint16_t y = (uint16_t)(77 * c.r + 150 * c.g + 29 * c.b) >> 8; // 0..255
            sumLuma += y;
        }
        uint8_t avg = sumLuma / NUM_LEDS;
        if (avg > AE_TARGET_LUMA && avg > 0) {
            uint8_t factor = (uint16_t)AE_TARGET_LUMA * 255 / avg;
            nscale8_video(leds, NUM_LEDS, factor);
        }
    }
    if (ENABLE_BROWN_GUARDRAIL) {
        for (int i = 0; i < NUM_LEDS; ++i) {
            CRGB &c = leds[i];
            if (c.r > c.g && c.g >= c.b) {
                uint8_t maxG = (uint16_t)c.r * MAX_G_PERCENT_OF_R / 100;
                uint8_t maxB = (uint16_t)c.r * MAX_B_PERCENT_OF_R / 100;
                if (c.g > maxG) c.g = maxG;
                if (c.b > maxB) c.b = maxB;
            }
        }
    }
    if (ENABLE_BLUR2D) {
        // Apply small blur across 9x9 grid using row/column passes
        // Row pass
        for (uint8_t y = 0; y < 9; ++y) {
            blur1d(&leds[y * 9], 9, BLUR2D_AMOUNT);
        }
        // Column pass
        for (uint8_t x = 0; x < 9; ++x) {
            CRGB col[9];
            for (uint8_t y = 0; y < 9; ++y) col[y] = leds[y * 9 + x];
            blur1d(col, 9, BLUR2D_AMOUNT);
            for (uint8_t y = 0; y < 9; ++y) leds[y * 9 + x] = col[y];
        }
    }
    applyVignette(leds);
    if (APPLY_GAMMA) {
        for (int i = 0; i < NUM_LEDS; ++i) napplyGamma_video(leds[i], GAMMA_VALUE);
    }
    FastLED.show();
    
    // End performance measurement
    metrics.endFrame();
    
    // Advance the global hue
    EVERY_N_MILLISECONDS(20) {
        gHue++;
    }
    
    // Performance metrics and status every 5 seconds
    EVERY_N_SECONDS(5) {
        // Create effect names array for metrics
        const char* effectNames[NUM_EFFECTS];
        for (uint8_t i = 0; i < NUM_EFFECTS; i++) {
            effectNames[i] = effects[i].name;
        }
        
        // Report performance metrics
        metrics.report(effectNames, NUM_EFFECTS);
        
        // Basic status
        Serial.print("Current: ");
        Serial.print(effects[currentEffect].name);
        Serial.print(", Palette: ");
        Serial.print(currentPaletteIndex);
        Serial.print(", FPS: ");
        Serial.print(metrics.getCurrentFPS(), 1);
        Serial.print(", Heap: ");
        Serial.print(ESP.getFreeHeap());
        Serial.println(" bytes");
    }
    
    // Frame rate
    delay(1000/120); // 120 FPS
}
