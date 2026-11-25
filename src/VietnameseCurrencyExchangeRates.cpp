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
char VietnameseCurrencyExchangeRates::api_url[128] = "https://currency-rates.zaitan.workers.dev/";

static void formatRateStr(int value, char *outStr, size_t maxLen) {
    if (value <= 0) {
        strncpy(outStr, "--", maxLen - 1);
        outStr[maxLen - 1] = '\0';
        return;
    }
    // Định dạng gọn gàng: 25580 -> "25.580" (dấu chấm 1px trên LED matrix)
    if (value >= 1000) {
        snprintf(outStr, maxLen, "%d.%03d", value / 1000, value % 1000);
    } else {
        snprintf(outStr, maxLen, "%d", value);
    }
    outStr[maxLen - 1] = '\0';
}

static String extractJsonString(const String &json, int startPos, const char *key) {
    String searchKey = "\"" + String(key) + "\":";
    int keyIdx = json.indexOf(searchKey, startPos);
    if (keyIdx == -1) return "";

    int startQuote = json.indexOf('"', keyIdx + searchKey.length());
    if (startQuote == -1) return "";

    int endQuote = json.indexOf('"', startQuote + 1);
    if (endQuote == -1) return "";

    return json.substring(startQuote + 1, endQuote);
}

static int extractJsonInt(const String &json, int startPos, const char *key) {
    String searchKey = "\"" + String(key) + "\":";
    int keyIdx = json.indexOf(searchKey, startPos);
    if (keyIdx == -1) return 0;

    int valStart = keyIdx + searchKey.length();
    while (valStart < (int)json.length() && (json[valStart] == ' ' || json[valStart] == '\t')) {
        valStart++;
    }

    int valEnd = valStart;
    while (valEnd < (int)json.length() && (isdigit(json[valEnd]) || json[valEnd] == '-')) {
        valEnd++;
    }

    if (valStart < valEnd) {
        return json.substring(valStart, valEnd).toInt();
    }
    return 0;
}

static bool parseRatePayload(const String &payload, CurrencyExchangeInfo &outInfo) {
    if (payload.length() == 0) return false;

    bool parsed = false;
    outInfo.count = 0;

    // 1. Kiểm tra trường "usd" hoặc "USD"
    int usdIdx = payload.indexOf("\"usd\":");
    if (usdIdx == -1) usdIdx = payload.indexOf("\"USD\":");

    if (usdIdx != -1) {
        int buy = extractJsonInt(payload, usdIdx, "buy");
        int sell = extractJsonInt(payload, usdIdx, "sell");
        String buyStr = extractJsonString(payload, usdIdx, "buy_str");
        String sellStr = extractJsonString(payload, usdIdx, "sell_str");

        strncpy(outInfo.usd.code, "USD", sizeof(outInfo.usd.code) - 1);
        outInfo.usd.code[sizeof(outInfo.usd.code) - 1] = '\0';
        outInfo.usd.buy = buy;
        outInfo.usd.sell = sell;
        outInfo.usd.has_buy = (buy > 0);
        outInfo.usd.has_sell = (sell > 0);

        if (buyStr.length() > 0) {
            strncpy(outInfo.usd.buy_str, buyStr.c_str(), sizeof(outInfo.usd.buy_str) - 1);
        } else {
            formatRateStr(buy, outInfo.usd.buy_str, sizeof(outInfo.usd.buy_str));
        }
        if (sellStr.length() > 0) {
            strncpy(outInfo.usd.sell_str, sellStr.c_str(), sizeof(outInfo.usd.sell_str) - 1);
        } else {
            formatRateStr(sell, outInfo.usd.sell_str, sizeof(outInfo.usd.sell_str));
        }
        outInfo.usd.buy_str[sizeof(outInfo.usd.buy_str) - 1] = '\0';
        outInfo.usd.sell_str[sizeof(outInfo.usd.sell_str) - 1] = '\0';

        outInfo.currencies[0] = outInfo.usd;
        outInfo.count = 1;
        parsed = true;
    }

    // 2. Parse các đồng tiền phổ biến: EUR, JPY, GBP, CNY, SGD, AUD, CAD, KRW
    const char *commonCodes[] = {"EUR", "JPY", "GBP", "CNY", "SGD", "AUD", "CAD", "KRW"};
    for (size_t i = 0; i < sizeof(commonCodes) / sizeof(commonCodes[0]); i++) {
        if (outInfo.count >= MAX_CURRENCIES) break;
        const char *cCode = commonCodes[i];
        String key = "\"" + String(cCode) + "\":";
        int cIdx = payload.indexOf(key);
        if (cIdx != -1) {
            int buy = extractJsonInt(payload, cIdx, "buy");
            int sell = extractJsonInt(payload, cIdx, "sell");
            String buyStr = extractJsonString(payload, cIdx, "buy_str");
            String sellStr = extractJsonString(payload, cIdx, "sell_str");

            if (buy > 0 || sell > 0) {
                CurrencyRate &cr = outInfo.currencies[outInfo.count];
                strncpy(cr.code, cCode, sizeof(cr.code) - 1);
                cr.code[sizeof(cr.code) - 1] = '\0';
                cr.buy = buy;
                cr.sell = sell;
                cr.has_buy = (buy > 0);
                cr.has_sell = (sell > 0);

                if (buyStr.length() > 0) {
                    strncpy(cr.buy_str, buyStr.c_str(), sizeof(cr.buy_str) - 1);
                } else {
                    formatRateStr(buy, cr.buy_str, sizeof(cr.buy_str));
                }
                if (sellStr.length() > 0) {
                    strncpy(cr.sell_str, sellStr.c_str(), sizeof(cr.sell_str) - 1);
                } else {
                    formatRateStr(sell, cr.sell_str, sizeof(cr.sell_str));
                }
                cr.buy_str[sizeof(cr.buy_str) - 1] = '\0';
                cr.sell_str[sizeof(cr.sell_str) - 1] = '\0';
                outInfo.count++;
            }
        }
    }

    String src = extractJsonString(payload, 0, "source");
    if (src.length() > 0) {
        strncpy(outInfo.source, src.c_str(), sizeof(outInfo.source) - 1);
        outInfo.source[sizeof(outInfo.source) - 1] = '\0';
    }

    String updated = extractJsonString(payload, 0, "updated_at");
    if (updated.length() > 0) {
        strncpy(outInfo.updated_time, updated.c_str(), sizeof(outInfo.updated_time) - 1);
        outInfo.updated_time[sizeof(outInfo.updated_time) - 1] = '\0';
    }

    return parsed;
}

static bool executeFetch(const char *url, CurrencyExchangeInfo &resultInfo) {
    if (!url || strlen(url) == 0) return false;

    String payload = "";
    bool fetchSuccess = false;
    bool isHttps = (strncmp(url, "https://", 8) == 0);

    HTTPClient http;
    http.setTimeout(15000);
    http.setConnectTimeout(15000);
    http.setUserAgent("ESP32-CYD-Updater");
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    if (isHttps) {
        uint32_t maxAlloc = ESP.getMaxAllocHeap();
        if (maxAlloc < 15 * 1024) {
            VCE_LOG_W("Heap too fragmented for TLS: maxAlloc=%d", maxAlloc);
            return false;
        }

        WiFiClientSecure client;
        client.setInsecure();
        client.setTimeout(15000);

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
        client.setTimeout(15000);

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
    CurrencyExchangeInfo *info = new CurrencyExchangeInfo();
    if (info) {
        if (executeFetch(api_url, *info)) {
            if (rateMutex != NULL && xSemaphoreTake(rateMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                info->is_valid = true;
                info->last_update_time = millis();
                current_info = *info;
                xSemaphoreGive(rateMutex);
                VCE_LOG_I("Exchange rates updated successfully: USD Buy=%s, Sell=%s", info->usd.buy_str, info->usd.sell_str);
            }
        } else {
            VCE_LOG_W("Fetch exchange rates failed");
        }
        delete info;
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
            8 * 1024,
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
