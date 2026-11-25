#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#define MAX_CURRENCIES 12

struct CurrencyRate {
  char code[8] = "";      // "USD", "EUR", "JPY", "GBP", "CNY", "KRW",...
  int buy = 0;            // Giá mua (vd: 25580)
  int sell = 0;           // Giá bán (vd: 25990)
  char buy_str[16] = "";  // "25.580"
  char sell_str[16] = ""; // "25.990"
  bool has_buy = false;
  bool has_sell = false;
};

struct CurrencyExchangeInfo {
  CurrencyRate usd; // Tỷ giá USD (ưu tiên số 1)
  CurrencyRate currencies[MAX_CURRENCIES];
  uint8_t count = 0;

  char source[32] = "Vietcombank";
  char updated_time[32] = "";
  bool is_valid = false;
  uint32_t last_update_time = 0;
};

class VietnameseCurrencyExchangeRates {
private:
  static CurrencyExchangeInfo current_info;
  static SemaphoreHandle_t rateMutex;
  static uint32_t last_fetch_time;
  static bool is_fetching;
  static uint32_t update_interval_ms;
  static char api_url[128];

  static void fetchRateTask(void *param);

public:
  static void
  init(const char *endpoint = "http://currency-rates.zaitan.workers.dev/");
  static void setApiUrl(const char *url);
  static void setUpdateInterval(uint32_t intervalMinutes);
  static void update(bool wifiConnected, bool force = false,
                     uint32_t intervalMinutes = 0);
  static bool fetchNow();
  static CurrencyExchangeInfo getInfo();

  // Tiện ích lấy nhanh tỷ giá USD
  static CurrencyRate getUSD();
  static bool hasUSDBuySell();

  // Tiện ích lấy bất kỳ ngoại tệ nào theo mã ("USD", "EUR", "JPY", "GBP",
  // "CNY", "KRW", "SGD",...)
  static CurrencyRate getRate(const char *currencyCode);

  static bool isFetching() { return is_fetching; }
};

// Alias thuận tiện
using CurrencyService = VietnameseCurrencyExchangeRates;
using ExchangeRateService = VietnameseCurrencyExchangeRates;
