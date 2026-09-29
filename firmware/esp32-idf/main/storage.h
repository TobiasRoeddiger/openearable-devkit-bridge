/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <nvs.h>
#include <string>

// Keep the existing namespace, key names, and NVS types across the SDK migration.
class AdapterStorage {
    nvs_handle_t handle{};
public:
    void begin() { ESP_ERROR_CHECK(nvs_open("oe-adapter", NVS_READWRITE, &handle)); }
    uint32_t getUInt(const char *key, uint32_t fallback) {
        uint32_t value;
        return nvs_get_u32(handle, key, &value) == ESP_OK ? value : fallback;
    }
    void putUInt(const char *key, uint32_t value) {
        ESP_ERROR_CHECK(nvs_set_u32(handle, key, value));
        ESP_ERROR_CHECK(nvs_commit(handle));
    }
    std::string getString(const char *key, const char *fallback) {
        char value[128]; size_t len = sizeof(value);
        return nvs_get_str(handle, key, value, &len) == ESP_OK ? value : fallback;
    }
    void putString(const char *key, const char *value) {
        ESP_ERROR_CHECK(nvs_set_str(handle, key, value));
        ESP_ERROR_CHECK(nvs_commit(handle));
    }
    size_t getBytes(const char *key, void *value, size_t size) {
        return nvs_get_blob(handle, key, value, &size) == ESP_OK ? size : 0;
    }
    void putBytes(const char *key, const void *value, size_t size) {
        ESP_ERROR_CHECK(nvs_set_blob(handle, key, value, size));
        ESP_ERROR_CHECK(nvs_commit(handle));
    }
    void remove(const char *key) {
        esp_err_t e = nvs_erase_key(handle, key);
        if (e != ESP_ERR_NVS_NOT_FOUND) ESP_ERROR_CHECK(e);
        ESP_ERROR_CHECK(nvs_commit(handle));
    }
};
