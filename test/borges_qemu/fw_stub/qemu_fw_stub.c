// QEMU has no SAR ADC, so the calibration poll in adc_hal_common.c never ends, and IDF runs
// it during startup, before setup(). Offset 0 is enough to pass. Never on a real device.
#include <stdbool.h>
#include <stdint.h>
uint32_t __wrap_adc_hal_self_calibration(int adc_n, int atten, bool internal_gnd) {
  (void)adc_n;
  (void)atten;
  (void)internal_gnd;
  return 0;
}

// Same reason at run time: analogRead (buttons, battery) polls the same event. Full scale
// reads as "no button pressed" on the resistor ladders.
bool __wrap_adc_oneshot_hal_convert(void* hal, int* outRaw) {
  (void)hal;
  *outRaw = 4095;
  return true;
}
