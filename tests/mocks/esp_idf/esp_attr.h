//minimal stand in for esp-idf's esp_attr.h so the ESP_PLATFORM code path can be compile checked on a pc
#ifndef MOCK_ESP_ATTR_H
#define MOCK_ESP_ATTR_H
#define IRAM_ATTR __attribute__((section(".iram1")))
#endif
