# VietnameseCurrencyExchangeRates

Thư viện ESP32 / Arduino cập nhật **Tỷ giá ngoại tệ** tại Việt Nam (bao gồm giá **Mua** và giá **Bán**) từ nguồn ngân hàng thương mại Việt Nam (Vietcombank).

Mặc định tối ưu hóa cho **USD (Đô la Mỹ)** với khả năng mở rộng linh hoạt sang **EUR, JPY, GBP, CNY, KRW, SGD, AUD, CAD...**

---

## ✨ Tính năng nổi bật

- 💵 **Đầy đủ Giá Mua & Giá Bán**: Tách bạch `USDM` (USD Mua) và `USDB` (USD Bán), tự động format dấu chấm/phẩy chuẩn cho màn hình LED Matrix.
- 🌐 **Đa ngoại tệ**: Dễ dàng truy xuất các ngoại tệ khác như `EUR`, `JPY`, `GBP`, `CNY`... qua hàm `getRate("EUR")`.
- ⚡ **Non-blocking FreeRTOS Task**: Lấy dữ liệu ngầm bằng FreeRTOS task (`VnCurTask`), không giật lag màn hình LED.
- 🛡️ **An toàn bộ nhớ**: Tối ưu RAM, sử dụng ArduinoJson v7 an toàn và ổn định.
- ⏱️ **Tùy biến linh hoạt**: Tùy chỉnh URL backend và chu kỳ cập nhật (mặc định 30 phút).

---

## 📦 Cài đặt

Thêm vào `platformio.ini`:
```ini
lib_deps =
    bblanchon/ArduinoJson@^7.0.3
    https://github.com/mvtcode/VietnameseCurrencyExchangeRates.git
```

---

## 🚀 Hướng dẫn sử dụng nhanh

```cpp
#include <Arduino.h>
#include <WiFi.h>
#include <VietnameseCurrencyExchangeRates.h>

const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

void setup() {
    Serial.begin(115200);
    delay(1000);

    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi Connected!");

    // Khởi tạo service tỷ giá
    VietnameseCurrencyExchangeRates::init();
    VietnameseCurrencyExchangeRates::setUpdateInterval(30); // 30 phút

    // Fetch ngay lần đầu
    if (VietnameseCurrencyExchangeRates::fetchNow()) {
        CurrencyRate usd = VietnameseCurrencyExchangeRates::getUSD();
        Serial.println("\n=== TỶ GIÁ NGOẠI TỆ ===");
        if (usd.has_buy && usd.has_sell) {
            Serial.printf("USDM (Mua): %s (%d d)\n", usd.buy_str, usd.buy);
            Serial.printf("USDB (Bán): %s (%d d)\n", usd.sell_str, usd.sell);
        } else {
            Serial.printf("USD: %s (%d d)\n", usd.sell_str, usd.sell);
        }

        // Mở rộng sang EUR nếu có
        CurrencyRate eur = VietnameseCurrencyExchangeRates::getRate("EUR");
        if (eur.sell > 0) {
            Serial.printf("EUR (Bán): %s (%d d)\n", eur.sell_str, eur.sell);
        }
    }
}

void loop() {
    // Non-blocking update định kỳ trong task ngầm
    VietnameseCurrencyExchangeRates::update(WiFi.status() == WL_CONNECTED);
    delay(1000);
}
```

---

## 📄 License
MIT License - Developed by [mvtcode](https://github.com/mvtcode).
