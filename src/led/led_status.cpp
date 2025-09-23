#include "led_status.h"

void ledInit(uint8_t, uint8_t){
  led.begin();
  led.setBrightness(50);
  led.show();
}

void updateLedState(LedMode mode){
  if (!ledOverride) currentLedMode = mode;
}

static void ledTask(void *){
  int brightness = 0; 
  bool up = true;

  for(;;){
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

      case LED_UPDATING: // 👈 cyan breathing (plus lent)
        // Cyan = G/B synchronisés ; distinct du GOOD/MODERATE
        led.setPixelColor(0, led.Color(0, brightness, brightness)); 
        led.show(); 
        break;
    }

    // Animation “breathing” avec vitesse différente pour UPDATING
    if (currentLedMode==LED_GOOD || currentLedMode==LED_MODERATE || currentLedMode==LED_UPDATING){
      // montée/descente
      brightness += (up ? 5 : -5);
      // bornes
      int maxB = 150;         // pic lumière
      int minB = 10;          // plancher
      if (brightness >= maxB){ brightness = maxB; up = false; }
      if (brightness <= minB){ brightness = minB; up = true; }

      // vitesse : UPDATING un poil plus lent → 40ms
      int delayMs = (currentLedMode==LED_UPDATING) ? 40 : 30;
      vTaskDelay(delayMs/portTICK_PERIOD_MS);

    } else if (currentLedMode==LED_BAD){
      // rouge fixe : tempo courte pour libérer le CPU
      brightness = 255; 
      up = true; 
      vTaskDelay(100/portTICK_PERIOD_MS);
    }
  }
}

void ledTaskStart(){ 
  xTaskCreatePinnedToCore(ledTask, "LED", 8192, 0, 1, 0, 1); 
}
