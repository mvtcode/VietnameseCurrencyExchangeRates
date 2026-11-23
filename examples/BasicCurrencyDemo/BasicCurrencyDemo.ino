#include <Arduino.h>
#include <WiFi.h>
#include <VietnameseCurrencyExchangeRates.h>

const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\nDang ket noi WiFi...");
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi Connected!");

    // 1. Khoi tao service
    VietnameseCurrencyExchangeRates::init();

    // 2. Fetch truc tiep lan dau
    Serial.println("Dang tai ty gia ngoai te...");
    if (VietnameseCurrencyExchangeRates::fetchNow()) {
        CurrencyExchangeInfo info = VietnameseCurrencyExchangeRates::getInfo();
        Serial.println("\n=== TY GIA NGOAI TE ===");
        Serial.printf("Nguon: %s\n", info.source);

        // Lay USD
        CurrencyRate usd = VietnameseCurrencyExchangeRates::getUSD();
        if (usd.has_buy && usd.has_sell) {
            Serial.printf("USDM (USD Mua): %s (%d d)\n", usd.buy_str, usd.buy);
            Serial.printf("USDB (USD Ban): %s (%d d)\n", usd.sell_str, usd.sell);
        } else {
            Serial.printf("USD: %s (%d d)\n", usd.sell_str, usd.sell);
        }

        // Hien thi cac ngoai te khac neu co
        for (uint8_t i = 0; i < info.count; i++) {
            if (strcasecmp(info.currencies[i].code, "USD") != 0) {
                Serial.printf("%s: Mua %s | Ban %s\n",
                    info.currencies[i].code,
                    info.currencies[i].buy_str,
                    info.currencies[i].sell_str
                );
            }
        }
    } else {
        Serial.println("Lay du lieu that bai!");
    }
}

void loop() {
    // Non-blocking update dinh ky trong FreeRTOS task
    VietnameseCurrencyExchangeRates::update(WiFi.status() == WL_CONNECTED);
    delay(2000);
}
