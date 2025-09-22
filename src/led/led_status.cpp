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
  int brightness = 0; bool up = true;
  for(;;){
    switch (currentLedMode) {
      case LED_BOOT:     led.setPixelColor(0, led.Color(0,0,150)); led.show(); vTaskDelay(1000/portTICK_PERIOD_MS); break;
      case LED_PAIRING:  led.setPixelColor(0, led.Color(0,0,150)); led.show(); vTaskDelay(300/portTICK_PERIOD_MS);
                         led.setPixelColor(0, 0); led.show(); vTaskDelay(300/portTICK_PERIOD_MS); break;
      case LED_GOOD:     led.setPixelColor(0, led.Color(0,brightness,0)); led.show(); break;
      case LED_MODERATE: led.setPixelColor(0, led.Color(brightness,brightness,0)); led.show(); break;
      case LED_BAD:      led.setPixelColor(0, led.Color(255,0,0)); led.show(); break;
    }
    if (currentLedMode==LED_GOOD || currentLedMode==LED_MODERATE){
      brightness += (up?5:-5);
      if (brightness>=150){ brightness=150; up=false; }
      if (brightness<=10){ brightness=10; up=true; }
      vTaskDelay(30/portTICK_PERIOD_MS);
    } else if (currentLedMode==LED_BAD){
      brightness=255; up=true; vTaskDelay(100/portTICK_PERIOD_MS);
    }
  }
}

void ledTaskStart(){ xTaskCreatePinnedToCore(ledTask, "LED", 8192, 0, 1, 0, 1); }
