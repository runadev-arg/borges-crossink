// Only in the qemu-fw build of the real firmware.
#include <stdbool.h>
#include <stdint.h>

// Built with this on: the firmware has no panel and no SD card here, so nothing it does
// on its own reports the heap. Off, for a plain power-on boot.
#define QEMU_FW_NETBOOT 1

#include <esp_heap_caps.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static void heapWatch(void* arg) {
  (void)arg;
  for (;;) {
    esp_rom_printf("@@ heap free=%u largest=%u min=%u tasks=%u\n",
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)uxTaskGetNumberOfTasks());
    TaskStatus_t tasks[16];
    UBaseType_t count = uxTaskGetSystemState(tasks, 16, NULL);
    for (UBaseType_t i = 0; i < count; ++i) {
      esp_rom_printf("@@   %s hwm=%u\n", tasks[i].pcTaskName, (unsigned)tasks[i].usStackHighWaterMark);
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}

__attribute__((constructor)) static void startHeapWatch(void) {
  xTaskCreate(heapWatch, "heapwatch", 3072, NULL, 5, NULL);
}

#if defined(QEMU_FW_NETBOOT)
// The token a silent restart leaves in RTC memory: Borges target, Status payload. Boots the
// firmware the way a real network session does.
extern uint32_t silentRebootMagic;
extern uint32_t silentRebootTarget;
extern uint32_t silentRebootPayload;
__attribute__((constructor)) static void qemuNetboot(void) {
  silentRebootMagic = 0xC1EAB007;
  silentRebootTarget = 8;
  silentRebootPayload = 3;
}
#endif
