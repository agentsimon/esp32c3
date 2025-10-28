#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <FastLED.h>
#include <LittleFS.h>
#include <freertos/task.h> // Required for vTaskDelay and vTaskPrioritySet
#include "esp_task_wdt.h" // Required for Task Watchdog Timer control

// --- Function Declarations (Prototypes) ---
// Declaring this function here ensures it's known before
// the setup() function (where it's called in web server handlers).
CRGB colorFromHexString(const char* hexString);
void updateSegmentPixels();
void updateLedBuffer();
// WDT Safe FastLED.show() wrapper
void showLeds(); 

// --- WiFi Configuration (Access Point Mode) ---
const char* ssid = "NeoPixel_Controller";
const char* password = "123456789";

// --- Web Server Object ---
AsyncWebServer server(80);
// --- NeoPixel Configuration for FastLED ---
const int LED_PIN = 4;
const int NUM_LEDS = 199;
const int NUM_SEGMENTS = 6;

#define LED_TYPE            WS2812B
#define COLOR_ORDER         GRB

CRGB leds[NUM_LEDS];
// --- NeoPixel Segment Definitions ---
int segmentLengths[NUM_SEGMENTS] = {16, 16, 16, 16, 16, 19};
int segmentStartPixel[NUM_SEGMENTS];
int segmentEndPixel[NUM_SEGMENTS];

CRGB currentSegmentColors[NUM_SEGMENTS];
// --- Per-Segment Flashing Variables ---
bool segmentIsFlashing[NUM_SEGMENTS] = {false};
unsigned long segmentFlashIntervalMillis[NUM_SEGMENTS];
unsigned long segmentLastFlashTime[NUM_SEGMENTS];
bool segmentCurrentFlashState[NUM_SEGMENTS];

// Global variable for overall brightness
uint8_t currentBrightness = 153; 

// --- Physical Button Configuration ---
const int BUTTON_PIN = 9;
volatile bool buttonToggleRequested = false;
volatile unsigned long lastButtonPressMillis = 0;
const unsigned long debounceDelay = 200;

// --- Global Strip State Variables ---
// Mode 0: OFF
// Mode 1: ON (Static/Segment Flashing)
// Mode 2: ON (Global ON/OFF Cycling)
// Mode 3: ON (Random Color Mode)
int currentStripMode = 0;

// --- Global ON/OFF Cycle Variables ---
unsigned long globalOnDurationMillis = 20000;
unsigned long globalOffDurationMillis = 2000;
unsigned long globalCycleLastToggleTime = 0;
bool globalCycleIsOnPhase = false;

// --- New Random Color Mode Variables ---
bool randomColorModeEnabled = false;
unsigned long randomSpeedMillis = 1000;
unsigned long lastRandomChangeTime = 0;

// --- WDT Fix: Helper function to safely call FastLED.show() ---
void showLeds() {
  // Use FreeRTOS suspend/resume on the current task (NULL)
  // to prevent the Task Watchdog Timer (TWDT) from resetting
  // during the blocking FastLED RMT transfer.
  vTaskSuspend(NULL); 

  FastLED.show();

  // Resume the task, allowing it to be watched again.
  vTaskResume(NULL);
}

// --- Helper function to recalculate segment pixel ranges ---
void updateSegmentPixels() {
    int currentPixel = 0;
    for (int i = 0; i < NUM_SEGMENTS; i++) {
        segmentStartPixel[i] = currentPixel;
        int effectiveLength = max(0, segmentLengths[i]);
        int rawEndPixel = currentPixel + effectiveLength - 1;

        segmentEndPixel[i] = min(rawEndPixel, NUM_LEDS - 1);
        if (currentPixel >= NUM_LEDS || effectiveLength == 0) {
            segmentStartPixel[i] = currentPixel;
            segmentEndPixel[i] = currentPixel - 1;
        }
        currentPixel = segmentEndPixel[i] + 1;
    } 
    Serial.println("Segment pixels updated.");
}

// --- Helper function to convert hex string to CRGB color ---
CRGB colorFromHexString(const char* hexString) {
    if (strlen(hexString) != 6) {
        return CRGB::Black;
    }
    unsigned long hexValue = strtol(hexString, NULL, 16);
    uint8_t r = (hexValue >> 16) & 0xFF;
    uint8_t g = (hexValue >> 8) & 0xFF;
    uint8_t b = hexValue & 0xFF;
    return CRGB(r, g, b);
}

// --- Function to build the LED buffer based on current segment states ---
void updateLedBuffer() {
    FastLED.clear();
    for (int seg = 0; seg < NUM_SEGMENTS; seg++) {
      if (segmentEndPixel[seg] >= segmentStartPixel[seg]) {
        for (int i = segmentStartPixel[seg]; i <= segmentEndPixel[seg]; i++) {
          if (i < NUM_LEDS) {
            if (segmentIsFlashing[seg] && !segmentCurrentFlashState[seg]) {
              leds[i] = CRGB::Black;
            } else {
              leds[i] = currentSegmentColors[seg];
            }
          }
        }
      }
    }
}

// --- Interrupt Service Routine (ISR) for the Button ---
void IRAM_ATTR handleButtonPress() {
    if (millis() - lastButtonPressMillis > debounceDelay) {
        buttonToggleRequested = true;
        lastButtonPressMillis = millis();
    }
}


void setup() {
  // Increase baud rate for faster logging
  Serial.begin(115200); 
  delay(100);
  Serial.println("\n--- ESP32 NeoPixel Controller Boot ---");

  // Elevate loop task priority to minimize WDT resets.
  vTaskPrioritySet(NULL, 2);
  Serial.println("Loop task priority set to 2.");
  // DIAGNOSTIC PRINT 1
  Serial.println("Starting FastLED setup...");

  // Initialize FastLED
  FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(currentBrightness);
  updateSegmentPixels();
  for (int i = 0; i < NUM_SEGMENTS; i++) {
      currentSegmentColors[i] = CRGB::Black;
      segmentLastFlashTime[i] = millis();
      segmentCurrentFlashState[i] = true;
      segmentFlashIntervalMillis[i] = 1000;
  }
  
  FastLED.clear();
  showLeds(); 
  Serial.println("FastLED setup complete.");
  Serial.println("Button set up complete.");
  // --- Button Setup ---
pinMode(BUTTON_PIN, INPUT_PULLUP);
attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), handleButtonPress, FALLING);
  
  // DIAGNOSTIC PRINT 2
  Serial.println("Starting WiFi AP...");

  // --- WiFi Setup ---
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  Serial.printf("AP IP address: %s\n", IP.toString().c_str());
  
  // DIAGNOSTIC PRINT 3
  Serial.println("Starting LittleFS mount...");
  
  // --- LittleFS Setup ---
  if (!LittleFS.begin(true)) {
    Serial.println("WARNING: LittleFS Mount Failed! Web server routes relying on files will fail.");
  } else {
    Serial.println("LittleFS mounted successfully.");
  }
  // DIAGNOSTIC PRINT 4
  Serial.println("Starting web server setup...");

  // --- Web Server Routes ---
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    if (LittleFS.exists("/index.html")) {
        request->send(LittleFS, "/index.html", "text/html");
    } else {
        request->send(404, "text/plain", "index.html not found on filesystem! (Check LittleFS status)");
    }
  });
  server.on("/ssid", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", ssid);
  });
  server.on("/setBrightness", HTTP_GET, [](AsyncWebServerRequest *request){
    if (request->hasArg("value")) {
      int brightnessPct = request->arg("value").toInt();
      currentBrightness = map(brightnessPct, 0, 100, 0, 255);
      FastLED.setBrightness(currentBrightness);
      showLeds();
      request->send(200, "text/plain", "Brightness set!");
    } else {
      request->send(400, "text/plain", "Brightness value missing");
    }
  });
  server.on("/setAllConfig", HTTP_GET, [](AsyncWebServerRequest *request){
    // FIX: Send response immediately to unblock HTTP thread
    request->send(200, "text/plain", "All configurations updated!");

    Serial.println("Received /setAllConfig command. Processing changes...");
    randomColorModeEnabled = false;
    
    // Parse Segment Data
    for (int i = 0; i < NUM_SEGMENTS; i++) {
      String paramColorName = "s" + String(i);
      if (request->hasArg(paramColorName)) {
        CRGB newColor = colorFromHexString(request->arg(paramColorName).c_str());
        currentSegmentColors[i] = newColor;
      }

      String paramLengthName = "l" + String(i);
    
      if (request->hasArg(paramLengthName)) {
        int newLength = request->arg(paramLengthName).toInt();
        if (newLength < 0) newLength = 0;
        if (newLength > NUM_LEDS) newLength = NUM_LEDS;
        segmentLengths[i] = newLength;
      }

      String paramFlashName = "f" + String(i);
      if (request->hasArg(paramFlashName)) {
          bool newIsFlashing = (request->arg(paramFlashName) == "true");
   
          if (newIsFlashing != segmentIsFlashing[i]) {
              segmentIsFlashing[i] = newIsFlashing;
              segmentCurrentFlashState[i] = true;
              segmentLastFlashTime[i] = millis();
          }
      }

      String paramFlashRateName = "fr" + String(i);
      if (request->hasArg(paramFlashRateName)) {
          float newFlashRateSeconds = request->arg(paramFlashRateName).toFloat();
          if (newFlashRateSeconds < 0.25) newFlashRateSeconds = 0.25;
          if (newFlashRateSeconds > 2.0) newFlashRateSeconds = 2.0;
          unsigned long newFlashRateMillis = (unsigned long)(newFlashRateSeconds * 1000.0);
          if (newFlashRateMillis != segmentFlashIntervalMillis[i]) {
              segmentFlashIntervalMillis[i] = newFlashRateMillis;
              segmentLastFlashTime[i] = millis();
              segmentCurrentFlashState[i] = true;
          }
      }
    }
    
    updateSegmentPixels();
    updateLedBuffer();
    showLeds();
    currentStripMode = 1;
  });
  server.on("/off", HTTP_GET, [](AsyncWebServerRequest *request){
    FastLED.clear();
    showLeds();
    currentStripMode = 0;
    randomColorModeEnabled = false;
    request->send(200, "text/plain", "Lights OFF");
  });
  server.on("/setAllColor", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!request->hasArg("color")) {
        request->send(400, "text/plain", "Color value missing");
        return;
    }
    String hexColor = request->arg("color");
    CRGB newColor = colorFromHexString(hexColor.c_str());
    for (int i = 0; i < NUM_LEDS; ++i) {
      leds[i] = newColor;
    }
    if (currentStripMode == 1 || currentStripMode == 2) {
        showLeds();
    }
 
    for (int i = 0; i < NUM_SEGMENTS; ++i) {
        segmentIsFlashing[i] = false;
    }
    randomColorModeEnabled = false;
    request->send(200, "text/plain", "All LEDs colour updated");
  });
  server.on("/random", HTTP_GET, [](AsyncWebServerRequest *request) {
    currentStripMode = 3;
    randomColorModeEnabled = true;
    randomSeed(analogRead(A0));
    request->send(200, "text/plain", "Random mode enabled!");
  });
  server.on("/setRandomSpeed", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasArg("value")) {
      float speedVal = request->arg("value").toFloat();
      // Speed value is typically 0.1 to 10 Hz (1000ms to 100ms interval)
      if (speedVal > 0.0) {
        randomSpeedMillis = (unsigned long)(1000 / speedVal);
      } else {
        randomSpeedMillis = 1000; // Default to 1 second if 0 or less
      }
      request->send(200, "text/plain", "Random speed set!");
    } else {
      request->send(400, "text/plain", "Speed value missing");
    }
  });
  server.begin();
  Serial.println("HTTP server started.");
}

void loop() {
  unsigned long currentMillis = millis();
  // Handle Button Toggle
  if (buttonToggleRequested) {
    buttonToggleRequested = false;
    currentStripMode = (currentStripMode + 1) % 4;
    Serial.printf("Strip mode changed to: %d\n", currentStripMode);
    if (currentStripMode == 0) {
      FastLED.clear();
      showLeds();
      randomColorModeEnabled = false;
    } else if (currentStripMode == 1) {
      for(int i = 0; i < NUM_SEGMENTS; ++i) {
          segmentCurrentFlashState[i] = true;
          segmentLastFlashTime[i] = currentMillis;
      }
      randomColorModeEnabled = false;
      updateLedBuffer();
      showLeds();
    } else if (currentStripMode == 2) {
      globalCycleIsOnPhase = true;
      globalCycleLastToggleTime = currentMillis;
      for(int i = 0; i < NUM_SEGMENTS; ++i) {
          segmentCurrentFlashState[i] = true;
          segmentLastFlashTime[i] = currentMillis;
      }
      randomColorModeEnabled = false;
      updateLedBuffer();
      showLeds();
    } else if (currentStripMode == 3) {
      randomColorModeEnabled = true;
      lastRandomChangeTime = currentMillis;
    }
  }

  // --- Mode 1: Static/Segment Flashing ---
  if (currentStripMode == 1) {
    bool needsUpdate = false;
    for (int i = 0; i < NUM_SEGMENTS; ++i) {
      if (segmentIsFlashing[i]) {
        if (currentMillis - segmentLastFlashTime[i] >= segmentFlashIntervalMillis[i]) {
          segmentLastFlashTime[i] = currentMillis;
          segmentCurrentFlashState[i] = !segmentCurrentFlashState[i];
          needsUpdate = true;
        }
      }
    }
    
    if (needsUpdate) {
      updateLedBuffer();
      showLeds();
    }
  }
  // --- Mode 2: Global ON/OFF Cycle ---
  else if (currentStripMode == 2) {
    bool needsUpdate = false;
    if (globalCycleIsOnPhase) {
      // ON phase
      if (currentMillis - globalCycleLastToggleTime >= globalOnDurationMillis) {
        // Time to turn OFF
        globalCycleIsOnPhase = false;
        globalCycleLastToggleTime = currentMillis;
        FastLED.clear();
        showLeds();
      } else {
        // Check for segment flashing updates only during ON phase
        for (int i = 0; i < NUM_SEGMENTS; ++i) {
          if (segmentIsFlashing[i]) {
            if (currentMillis - segmentLastFlashTime[i] >= segmentFlashIntervalMillis[i]) {
              segmentLastFlashTime[i] = currentMillis;
              segmentCurrentFlashState[i] = !segmentCurrentFlashState[i];
              needsUpdate = true;
            }
          }
        }
        if (needsUpdate) {
          updateLedBuffer();
          showLeds();
        }
      }
    } else {
      // OFF phase
      if (currentMillis - globalCycleLastToggleTime >= globalOffDurationMillis) {
        // Time to turn ON
        globalCycleIsOnPhase = true;
        globalCycleLastToggleTime = currentMillis;
        for(int i = 0; i < NUM_SEGMENTS; ++i) {
            segmentCurrentFlashState[i] = true;
            segmentLastFlashTime[i] = currentMillis;
        }
        updateLedBuffer();
        showLeds();
      }
    }
  }
  // --- Mode 3: Random Color Mode ---
  else if (currentStripMode == 3 && randomColorModeEnabled) {
    if (currentMillis - lastRandomChangeTime >= randomSpeedMillis) {
      lastRandomChangeTime = currentMillis;
      for (int i = 0; i < NUM_LEDS; ++i) {
        leds[i] = CHSV(random8(), 255, 255);
      }
      showLeds();
    }
  }
  
  // --- Mandatory WDT Feed and Context Switch ---
  // Using pdMS_TO_TICKS(5) instead of vTaskDelay(1) provides a more reliable
  // context switch and WDT feeding without hogging the CPU.
  vTaskDelay(pdMS_TO_TICKS(5));
}
