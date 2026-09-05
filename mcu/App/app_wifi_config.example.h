/**
 * @file    app_wifi_config.example.h
 * @brief   WiFi / NTP / weather configuration example
 * @note    Copy this file to app_wifi_config.h and fill in your own values.
 *          app_wifi_config.h is ignored by Git to avoid leaking credentials.
 */
#ifndef __APP_WIFI_CONFIG_EXAMPLE_H
#define __APP_WIFI_CONFIG_EXAMPLE_H

#define WIFI_ENABLED               1
#define WIFI_SSID                  "Your2GHotspot"
#define WIFI_PASSWORD              "YourPassword"
#define WIFI_CONNECT_TIMEOUT_MS    20000
#define WIFI_RETRY_DELAY_MS        10000

#define NTP_ENABLED                1
#define NTP_SERVER                 "ntp.aliyun.com"
#define NTP_TIMEZONE               8
#define NTP_SYNC_ON_BOOT           1

#define WEATHER_ENABLED            1
#define WEATHER_API_KEY            "YourWeatherApiKey"
#define WEATHER_CITY               "hangzhou"
#define WEATHER_LANGUAGE           "en"
#define WEATHER_UNIT               "c"
#define WEATHER_REFRESH_MS         1800000
#define WEATHER_RETRY_MS           300000

#endif /* __APP_WIFI_CONFIG_EXAMPLE_H */
