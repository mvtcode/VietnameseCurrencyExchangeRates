#include "VietnameseCurrencyExchangeRates.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#if defined(DEBUG_VIETNAMESE_RATES) || defined(ENABLE_SERIAL_LOG)
#define VCE_LOG_I(fmt, ...) Serial.printf("[CurrencyRates][I] " fmt "\n", ##__VA_ARGS__)
#define VCE_LOG_W(fmt, ...) Serial.printf("[CurrencyRates][W] " fmt "\n", ##__VA_ARGS__)
#define VCE_LOG_E(fmt, ...) Serial.printf("[CurrencyRates][E] " fmt "\n", ##__VA_ARGS__)
#else
#define VCE_LOG_I(...)
#define VCE_LOG_W(...)
#define VCE_LOG_E(...)
#endif

CurrencyExchangeInfo VietnameseCurrencyExchangeRates::current_info;
SemaphoreHandle_t VietnameseCurrencyExchangeRates::rateMutex = NULL;
uint32_t VietnameseCurrencyExchangeRates::last_fetch_time = 0;
bool VietnameseCurrencyExchangeRates::is_fetching = false;
uint32_t VietnameseCurrencyExchangeRates::update_interval_ms = 30 * 60 * 1000; // 30 phút
char VietnameseCurrencyExchangeRates::api_url[128] = "http://currency-rates.zaitan.workers.dev/";

static void formatRateStr(int value, char *outStr, size_t maxLen) {
    if (value <= 0) {
        strncpy(outStr, "--", maxLen - 1);
        return;
    }
    // Định dạng gọn gàng: 25580 -> "25.580" (dấu chấm 1px trên LED matrix)
    if (value >= 1000) {
        snprintf(outStr, maxLen, "%d.%03d", value / 1000, value % 1000);
    } else {
        snprintf(outStr, maxLen, "%d", value);
    }
}

static void parseCurrencyObject(JsonObject obj, const char *code, CurrencyRate &outRate) {
    strncpy(outRate.code, code, sizeof(outRate.code) - 1);
    outRate.code[sizeof(outRate.code) - 1] = '\0';

    int buyVal = obj["buy"] | 0;
    int sellVal = obj["sell"] | (obj["rate"] | 0);

    if (buyVal > 0) {
        outRate.buy = buyVal;
        outRate.has_buy = true;
        formatRateStr(buyVal, outRate.buy_str, sizeof(outRate.buy_str));
    } else {
        outRate.buy = 0;
        outRate.has_buy = false;
        outRate.buy_str[0] = '\0';
    }

    if (sellVal > 0) {
        outRate.sell = sellVal;
        outRate.has_sell = true;
        formatRateStr(sellVal, outRate.sell_str, sizeof(outRate.sell_str));
    } else {
        outRate.sell = 0;
        outRate.has_sell = false;
        outRate.sell_str[0] = '\0';
    }
}

static bool parseRatePayload(const String &payload, CurrencyExchangeInfo &outInfo) {
    if (payload.length() == 0) return false;

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
        VCE_LOG_E("JSON parse failed: %s", error.c_str());
        return false;
    }

    bool parsed = false;
    outInfo.count = 0;

    // 1. Kiểm tra trường "usd" hoặc "USD"
    if (doc["usd"].is<JsonObject>()) {
        parseCurrencyObject(doc["usd"].as<JsonObject>(), "USD", outInfo.usd);
        parsed = true;
    } else if (doc["USD"].is<JsonObject>()) {
        parseCurrencyObject(doc["USD"].as<JsonObject>(), "USD", outInfo.usd);
        parsed = true;
    } else if (doc["buy"].is<int>() || doc["sell"].is<int>()) {
        // Cấu trúc phẳng root { buy: 25580, sell: 25990 }
        parseCurrencyObject(doc.as<JsonObject>(), "USD", outInfo.usd);
        parsed = true;
    } else if (doc["rates"].is<JsonObject>()) {
        // Fallback OpenExchangeRates { rates: { VND: 25779 } }
        int v = (int)round(doc["rates"]["VND"] | 0.0f);
        if (v > 0) {
            strncpy(outInfo.usd.code, "USD", sizeof(outInfo.usd.code) - 1);
            outInfo.usd.sell = v;
            outInfo.usd.has_sell = true;
            formatRateStr(v, outInfo.usd.sell_str, sizeof(outInfo.usd.sell_str));
            parsed = true;
        }
    }

    // Luôn lưu USD vào vị trí đầu tiên của mảng currencies
    if (parsed) {
        outInfo.currencies[0] = outInfo.usd;
        outInfo.count = 1;
    }

    // 2. Bóc tách danh mục currencies nếu có { "currencies": { "EUR": {...}, "JPY": {...} } }
    JsonObject currObj;
    if (doc["currencies"].is<JsonObject>()) {
        currObj = doc["currencies"].as<JsonObject>();
    } else if (doc["data"].is<JsonObject>()) {
        currObj = doc["data"].as<JsonObject>();
    }

    if (!currObj.isNull()) {
        for (JsonPair kv : currObj) {
            if (outInfo.count >= MAX_CURRENCIES) break;

            const char *cCode = kv.key().c_str();
            // Nếu là USD thì đã xử lý ở trên rồi, bỏ qua tránh trùng lặp
            if (strcasecmp(cCode, "USD") == 0) continue;

            if (kv.value().is<JsonObject>()) {
                CurrencyRate &cr = outInfo.currencies[outInfo.count];
                parseCurrencyObject(kv.value().as<JsonObject>(), cCode, cr);
                if (cr.buy > 0 || cr.sell > 0) {
                    outInfo.count++;
                }
            }
        }
    }

    const char *src = doc["source"] | "Vietcombank";
    strncpy(outInfo.source, src, sizeof(outInfo.source) - 1);

    const char *updated = doc["updated_at"] | "";
    strncpy(outInfo.updated_time, updated, sizeof(outInfo.updated_time) - 1);

    return parsed;
}

static bool executeFetch(const char *url, CurrencyExchangeInfo &resultInfo) {
    if (!url || strlen(url) == 0) return false;

    String payload = "";
    bool fetchSuccess = false;
    bool isHttps = (strncmp(url, "https://", 8) == 0);

    HTTPClient http;
    http.setTimeout(20000);
    http.setConnectTimeout(20000);
    http.setUserAgent("ESP32-VietnameseCurrencyExchangeRates/1.0");
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (isHttps) {
        uint32_t maxAlloc = ESP.getMaxAllocHeap();
        if (maxAlloc < 15 * 1024) {
            VCE_LOG_W("Heap too fragmented for TLS: maxAlloc=%d", maxAlloc);
            return false;
        }

        WiFiClientSecure client;
        client.setInsecure();
        client.setTimeout(15);
        static const char *alpn[] = {"http/1.1", NULL};
        client.setAlpnProtocols(alpn);

        VCE_LOG_I("Fetching exchange rates (HTTPS) from: %s", url);
        if (http.begin(client, url)) {
            int httpCode = http.GET();
            if (httpCode == HTTP_CODE_OK) {
                payload = http.getString();
                fetchSuccess = true;
                VCE_LOG_I("FX fetch OK, length: %d bytes", payload.length());
            } else {
                char errBuf[128] = {0};
                client.lastError(errBuf, sizeof(errBuf));
                VCE_LOG_W("HTTP Error: %d, TLS error: %s", httpCode, errBuf);
            }
            http.end();
        }
        client.stop();
    } else {
        WiFiClient client;
        client.setTimeout(15);

        VCE_LOG_I("Fetching exchange rates (HTTP) from: %s", url);
        if (http.begin(client, url)) {
            int httpCode = http.GET();
            if (httpCode == HTTP_CODE_OK) {
                payload = http.getString();
                fetchSuccess = true;
                VCE_LOG_I("FX fetch OK, length: %d bytes", payload.length());
            } else {
                VCE_LOG_W("HTTP Error: %d", httpCode);
            }
            http.end();
        }
        client.stop();
    }

    if (fetchSuccess && payload.length() > 0) {
        return parseRatePayload(payload, resultInfo);
    }
    return false;
}

void VietnameseCurrencyExchangeRates::init(const char *endpoint) {
    if (rateMutex == NULL) {
        rateMutex = xSemaphoreCreateMutex();
    }
    if (endpoint && strlen(endpoint) > 0) {
        strncpy(api_url, endpoint, sizeof(api_url) - 1);
    }
    last_fetch_time = 0;
    is_fetching = false;
    VCE_LOG_I("Init VietnameseCurrencyExchangeRates with URL: %s", api_url);
}

void VietnameseCurrencyExchangeRates::setApiUrl(const char *url) {
    if (url && strlen(url) > 0) {
        strncpy(api_url, url, sizeof(api_url) - 1);
    }
}

void VietnameseCurrencyExchangeRates::setUpdateInterval(uint32_t intervalMinutes) {
    if (intervalMinutes < 5) intervalMinutes = 5;
    update_interval_ms = intervalMinutes * 60 * 1000;
}

void VietnameseCurrencyExchangeRates::fetchRateTask(void *param) {
    CurrencyExchangeInfo info;
    if (executeFetch(api_url, info)) {
        if (rateMutex != NULL && xSemaphoreTake(rateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            info.is_valid = true;
            info.last_update_time = millis();
            current_info = info;
            xSemaphoreGive(rateMutex);
            VCE_LOG_I("Exchange rates updated successfully: USD Buy=%s, Sell=%s", info.usd.buy_str, info.usd.sell_str);
        }
    } else {
        VCE_LOG_W("Fetch exchange rates failed");
    }
    is_fetching = false;
    vTaskDelete(NULL);
}

void VietnameseCurrencyExchangeRates::update(bool wifiConnected, bool force, uint32_t intervalMinutes) {
    if (!wifiConnected) return;

    uint32_t interval = (intervalMinutes > 0) ? (intervalMinutes * 60 * 1000) : update_interval_ms;
    if (!current_info.is_valid) {
        interval = 20 * 1000; // Retry nhanh sau 20s nếu chưa có dữ liệu
    }

    uint32_t now = millis();
    if (!is_fetching && (force || last_fetch_time == 0 || (now - last_fetch_time >= interval))) {
        last_fetch_time = now;
        is_fetching = true;

        BaseType_t res = xTaskCreatePinnedToCore(
            fetchRateTask,
            "VnCurTask",
            6 * 1024,
            NULL,
            1,
            NULL,
            0
        );
        if (res != pdPASS) {
            VCE_LOG_E("Task VnCurTask creation failed");
            is_fetching = false;
        }
    }
}

bool VietnameseCurrencyExchangeRates::fetchNow() {
    CurrencyExchangeInfo info;
    if (executeFetch(api_url, info)) {
        if (rateMutex != NULL && xSemaphoreTake(rateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            info.is_valid = true;
            info.last_update_time = millis();
            current_info = info;
            xSemaphoreGive(rateMutex);
            return true;
        }
    }
    return false;
}

CurrencyExchangeInfo VietnameseCurrencyExchangeRates::getInfo() {
    CurrencyExchangeInfo copy;
    if (rateMutex != NULL && xSemaphoreTake(rateMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        copy = current_info;
        xSemaphoreGive(rateMutex);
    }
    return copy;
}

CurrencyRate VietnameseCurrencyExchangeRates::getUSD() {
    CurrencyExchangeInfo info = getInfo();
    return info.usd;
}

bool VietnameseCurrencyExchangeRates::hasUSDBuySell() {
    CurrencyRate usd = getUSD();
    return (usd.has_buy && usd.has_sell && usd.buy > 0 && usd.sell > 0);
}

CurrencyRate VietnameseCurrencyExchangeRates::getRate(const char *currencyCode) {
    CurrencyRate res;
    if (!currencyCode || strlen(currencyCode) == 0) return res;

    CurrencyExchangeInfo info = getInfo();
    if (!info.is_valid || info.count == 0) return res;

    // Tìm trong danh sách ngoại tệ
    for (uint8_t i = 0; i < info.count; i++) {
        if (strcasecmp(info.currencies[i].code, currencyCode) == 0) {
            return info.currencies[i];
        }
    }
    return res;
}
