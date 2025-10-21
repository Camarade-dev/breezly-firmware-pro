#include "led_status.h"
#include <Arduino.h>
// (assume: objet global Adafruit_NeoPixel led; currentLedMode/ledOverride existent)

static TaskHandle_t s_ledTask = nullptr;
static volatile bool s_ledMuted = false;

void ledInit(uint8_t, uint8_t){
  led.begin();
  led.setBrightness(50);
  led.show();
}

bool ledIsMuted(){ return s_ledMuted; }

// Coupe toute activité LED (aucun show RMT pendant TLS/OTA)
void ledSuspend(){
  s_ledMuted = true;
  // Option dur : gel de la tâche (facultatif si le check dans la loop suffit)
  if (s_ledTask) vTaskSuspend(s_ledTask);
  // NE PAS faire de led.show() ici → on veut zéro RMT pendant l’OTA
}

// Relance l’anim LED après l’OTA (si pas de reboot)
void ledResume(){
  if (s_ledTask) vTaskResume(s_ledTask);
  s_ledMuted = false;
}

void updateLedState(LedMode mode){
  if (!ledOverride) currentLedMode = mode;
}

static void ledTask(void *){
  int brightness = 0; 
  bool up = true;

  for(;;){

    // --- MUTE GLOBAL : aucune écriture RMT tant que c'est vrai ---
    if (s_ledMuted){
      vTaskDelay(40/portTICK_PERIOD_MS);
      continue;
    }

    switch (currentLedMode) {
      case LED_BOOT:
        led.setPixelColor(0, led.Color(0,0,150)); 
        led.show(); 
        vTaskDelay(1000/portTICK_PERIOD_MS); 
        break;

      case LED_PAIRING:
        led.setPixelColor(0, led.Color(0,0,150)); 
        led.show(); 
        vTaskDelay(300/portTICK_PERIOD_MS);
        led.setPixelColor(0, 0); 
        led.show(); 
        vTaskDelay(300/portTICK_PERIOD_MS); 
        break;

      case LED_GOOD:     // vert breathing
        led.setPixelColor(0, led.Color(0,brightness,0)); 
        led.show(); 
        break;

      case LED_MODERATE: // jaune breathing
        led.setPixelColor(0, led.Color(brightness,brightness,0)); 
        led.show(); 
        break;

      case LED_BAD:      // rouge fixe
        led.setPixelColor(0, led.Color(255,0,0)); 
        led.show(); 
        break;

      case LED_UPDATING: // cyan breathing (lent)
        led.setPixelColor(0, led.Color(0, brightness, brightness)); 
        led.show(); 
        break;
    }

    // Breathing
    if (currentLedMode==LED_GOOD || currentLedMode==LED_MODERATE || currentLedMode==LED_UPDATING){
      brightness += (up ? 2 : -2);
      int maxB = 150, minB = 10;
      if (brightness >= maxB){ brightness = maxB; up = false; }
      if (brightness <= minB){ brightness = minB; up = true; }
      int delayMs = (currentLedMode==LED_UPDATING) ? 30 : 40;
      vTaskDelay(delayMs/portTICK_PERIOD_MS);
    } else if (currentLedMode==LED_BAD){
      brightness = 255; 
      up = true; 
      vTaskDelay(100/portTICK_PERIOD_MS);
    }
  }
}

void ledTaskStart(){ 
  // >>> garde le handle pour suspend/resume
  xTaskCreatePinnedToCore(ledTask, "LED", 8192, nullptr, 1, &s_ledTask, 1);
}
