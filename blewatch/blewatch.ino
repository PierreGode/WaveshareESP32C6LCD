#include "Display_ST7789.h"
#include "LVGL_Driver.h"

void setup() {
  LCD_Init();
  Lvgl_Init();
}

void loop() {
  Timer_Loop();
  delay(5);
}
