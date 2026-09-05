/*
 * Portfolio excerpt from the original freertos.c.
 * Functions below are copied verbatim from the application project.
 * This excerpt is for code review and is not intended to replace the full CubeMX file.
 */

/* ===== MX_FREERTOS_Init ===== */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* Create the queue(s) */
  /* creation of myQueue01 */
  myQueue01Handle = osMessageQueueNew (8, sizeof(AppMessage_t), &myQueue01_attributes);

  /* creation of myQueue04 */
  myQueue04Handle = osMessageQueueNew (4, sizeof(AppMessage_t), &myQueue04_attributes);

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);
  guiTaskHandle = osThreadNew(StartTaskGui, NULL, &guiTask_attributes);
  keyTaskHandle = osThreadNew(StartTaskKey, NULL, &keyTask_attributes);
  uartTaskHandle = osThreadNew(StartTaskUart, NULL, &uartTask_attributes);
  otherTaskHandle = osThreadNew(StartTaskOther, NULL, &otherTask_attributes);
  nfcTaskHandle = osThreadNew(StartTaskNFC, NULL, &nfcTask_attributes);
  wifiTaskHandle = osThreadNew(StartWifiTask, NULL, &wifiTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */
}

/* ===== Issue_GetPayloadTarget ===== */
static uint8_t Issue_GetPayloadTarget(uint8_t payloadBlock, uint8_t *sector, uint8_t *block)
{
    if (payloadBlock > 43) return 0;

    static const struct { uint8_t sector; uint8_t block; } map[44] = {
        {1, 0}, {1, 1}, {1, 2},
        {2, 0}, {2, 1}, {2, 2},
        {3, 0}, {3, 1}, {3, 2},
        {4, 0}, {4, 1}, {4, 2},
        {5, 0}, {5, 1}, {5, 2},
        {6, 0}, {6, 1}, {6, 2},
        {7, 0}, {7, 1}, {7, 2},
        {8, 0}, {8, 1}, {8, 2},
        {9, 0}, {9, 1}, {9, 2},
        {10, 0}, {10, 1}, {10, 2},
        {11, 0}, {11, 1}, {11, 2},
        {12, 0},
        {12, 1}, {12, 2},
        {13, 0}, {13, 1}, {13, 2},
        {14, 0}, {14, 1}, {14, 2},
        {15, 0}, {15, 1},
    };

    *sector = map[payloadBlock].sector;
    *block  = map[payloadBlock].block;

    if (*block == 3 || (*sector == 0 && *block == 0))
    {
        return 0;
    }

    return 1;
}

/* ===== Issue_WriteCardWithPayload ===== */
static uint8_t Issue_WriteCardWithPayload(const uint8_t uid[4])
{
    const uint8_t defaultKey[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t buf[16];

    Issue_BuildCardHeader(buf, uid);

    if (RC522_AuthState(RC522_PICC_AUTHENT1A, 0x03, (uint8_t *)defaultKey, uid) != RC522_OK)
    {
        return ISSUE_ERR_AUTH;
    }
    if (RC522_WriteBlock(0, 1, buf) != RC522_OK)
    {
        return ISSUE_ERR_WRITE;
    }

    for (uint8_t pb = 0; pb < 44; pb++)
    {
        uint8_t sector, block;
        if (!Issue_GetPayloadTarget(pb, &sector, &block))
        {
            return ISSUE_ERR_FAIL;
        }

        uint8_t authBlock = (uint8_t)(sector * 4 + 3);
        if (RC522_AuthState(RC522_PICC_AUTHENT1A, authBlock, (uint8_t *)defaultKey, uid) != RC522_OK)
        {
            return ISSUE_ERR_AUTH;
        }

        memcpy(buf, &s_issuePayload[pb * 16], 16);
        if (RC522_WriteBlock(sector, block, buf) != RC522_OK)
        {
            return ISSUE_ERR_WRITE;
        }
    }

    return ISSUE_ERR_NONE;
}

/* ===== Issue_VerifyCardWithPayload ===== */
static uint8_t Issue_VerifyCardWithPayload(const uint8_t uid[4])
{
    const uint8_t defaultKey[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t expected[16];
    uint8_t actual[16];

    Issue_BuildCardHeader(expected, uid);

    if (RC522_AuthState(RC522_PICC_AUTHENT1A, 0x03, (uint8_t *)defaultKey, uid) != RC522_OK)
    {
        return ISSUE_ERR_AUTH;
    }
    if (RC522_ReadBlock(0, 1, actual) != RC522_OK)
    {
        return ISSUE_ERR_VERIFY;
    }
    if (memcmp(expected, actual, 16) != 0)
    {
        return ISSUE_ERR_VERIFY;
    }

    for (uint8_t pb = 0; pb < 44; pb++)
    {
        uint8_t sector, block;
        if (!Issue_GetPayloadTarget(pb, &sector, &block))
        {
            return ISSUE_ERR_FAIL;
        }

        uint8_t authBlock = (uint8_t)(sector * 4 + 3);
        if (RC522_AuthState(RC522_PICC_AUTHENT1A, authBlock, (uint8_t *)defaultKey, uid) != RC522_OK)
        {
            return ISSUE_ERR_AUTH;
        }

        if (RC522_ReadBlock(sector, block, actual) != RC522_OK)
        {
            return ISSUE_ERR_VERIFY;
        }

        memcpy(expected, &s_issuePayload[pb * 16], 16);
        if (memcmp(expected, actual, 16) != 0)
        {
            return ISSUE_ERR_VERIFY;
        }
    }

    return ISSUE_ERR_NONE;
}

/* ===== Issue_WaitAndWriteCard ===== */
static uint8_t Issue_WaitAndWriteCard(void)
{
    const uint32_t timeoutTicks = pdMS_TO_TICKS(10000U);
    uint32_t startTick = osKernelGetTickCount();
    uint8_t uid[4] = {0};

    while (1)
    {
        if (RC522_ScanCard(uid) == RC522_OK)
        {
            break;
        }

        if ((osKernelGetTickCount() - startTick) >= timeoutTicks)
        {
            return ISSUE_ERR_NO_CARD;
        }

        osDelay(100);
    }

    uint8_t err = Issue_WriteCardWithPayload(uid);
    if (err != ISSUE_ERR_NONE)
    {
        RC522_Halt();
        return err;
    }

    err = Issue_VerifyCardWithPayload(uid);
    if (err != ISSUE_ERR_NONE)
    {
        RC522_Halt();
        return err;
    }

    RC522_Halt();
    return ISSUE_ERR_NONE;
}

/* ===== NFC_ValidateCard ===== */
static uint8_t NFC_ValidateCard(uint8_t *uid)
{
    const uint8_t defaultKey[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t buf[16];
    uint16_t sum = 0;

    if (RC522_AuthState(RC522_PICC_AUTHENT1A, 0x03, (uint8_t *)defaultKey, uid) != RC522_OK)
    {
        return 0;
    }

    if (RC522_Read(1, buf) != RC522_OK)
    {
        return 0;
    }

    for (uint8_t i = 0; i < 14; i++)
    {
        sum += buf[i];
    }

    uint16_t storedChecksum = (uint16_t)buf[14] | ((uint16_t)buf[15] << 8);
    if (sum != storedChecksum)
    {
        return 0;
    }

    if (memcmp(buf, uid, 4) != 0)
    {
        return 0;
    }

    uint8_t cardType = buf[12];
    if (cardType > 2)
    {
        return 0;
    }

    return cardType + 1;
}

/* ===== Weather_UpdateOnce ===== */
static void Weather_UpdateOnce(void)
{
#if WEATHER_ENABLED
    if (!g_wifiConnected)
    {
        g_weatherState = WEATHER_STATE_WAIT_WIFI;
        g_weatherValid = 0;
        snprintf(g_weatherText, sizeof(g_weatherText), "WX wait WiFi");
        return;
    }

    g_weatherState = WEATHER_STATE_QUERYING;
    g_weatherValid = 0;

    char city[16];
    char textDay[32];
    char high[8];
    char textNight[32];
    char low[8];
    char precip[8];

    memset(city, 0, sizeof(city));
    memset(textDay, 0, sizeof(textDay));
    memset(high, 0, sizeof(high));
    memset(textNight, 0, sizeof(textNight));
    memset(low, 0, sizeof(low));
    memset(precip, 0, sizeof(precip));

    int ret = ESP01S_QueryWeather(
        WEATHER_API_KEY,
        WEATHER_CITY,
        WEATHER_LANGUAGE,
        WEATHER_UNIT,
        city, sizeof(city),
        textDay, sizeof(textDay),
        high, sizeof(high),
        textNight, sizeof(textNight),
        low, sizeof(low),
        precip, sizeof(precip)
    );

    if (ret == 0)
    {
        char cityShort[4];
        cityShort[0] = city[0];
        cityShort[1] = (city[1] != '\0') ? city[1] : ' ';
        cityShort[2] = '\0';

        const char *weatherShort = Weather_AbbrevText(textDay);
        const char *h = (high[0] != '\0') ? high : "--";
        const char *l = (low[0]  != '\0') ? low  : "--";

        snprintf(g_weatherText, sizeof(g_weatherText),
                 "%s %s %s/%sC",
                 cityShort, weatherShort, h, l);

        g_weatherValid = 1;
        g_weatherState = WEATHER_STATE_VALID;
    }
    else
    {
        g_weatherValid = 0;
        g_weatherState = WEATHER_STATE_FAILED;
        snprintf(g_weatherText, sizeof(g_weatherText), "WX unavailable");
    }
#else
    g_weatherState = WEATHER_STATE_DISABLED;
    g_weatherValid = 0;
    g_weatherText[0] = '\0';
#endif
}

/* ===== StartWifiTask ===== */
void StartWifiTask(void *argument)
{
#if WIFI_ENABLED
  while (!g_uart6DrvReady)
  {
    osDelay(100);
  }

  osDelay(3000);

  for (;;)
  {
    g_wifiState = WIFI_STATE_INIT;
    g_wifiConnected = 0;

    ESP01S_Init(&s_uart6Drv);
    ESP01S_SetWiFi(WIFI_SSID, WIFI_PASSWORD);
    ESP01S_SetTcpServer("", 0);

#if NTP_ENABLED
    ESP01S_SetNtpServer(NTP_SERVER, NTP_TIMEZONE);
    g_ntpState = NTP_STATE_WAIT_WIFI;
#else
    g_ntpState = NTP_STATE_DISABLED;
    g_ntpSynced = 0;
#endif

    g_wifiState = WIFI_STATE_CONNECTING;

    int ret = ESP01S_Start();

    if (ESP01S_IsWiFiConnected())
    {
      g_wifiState = WIFI_STATE_CONNECTED;
      g_wifiConnected = 1;

#if NTP_ENABLED
      g_ntpState = NTP_STATE_SYNCING;

      if (ESP01S_IsNtpSynced())
      {
        int rtcRet = ESP01S_SetRtcFromNtp(&hrtc);
        if (rtcRet == 0)
        {
          g_ntpSynced = 1;
          g_ntpState = NTP_STATE_SYNCED;
        }
        else
        {
          g_ntpSynced = 0;
          g_ntpState = NTP_STATE_FAILED;
        }
      }
      else
      {
        g_ntpSynced = 0;
        g_ntpState = NTP_STATE_FAILED;
      }
#endif

#if WEATHER_ENABLED
      Weather_UpdateOnce();
#endif

      {
        TickType_t lastWeatherTick = xTaskGetTickCount();

        while (ESP01S_IsWiFiConnected())
        {
#if WEATHER_ENABLED
          TickType_t now = xTaskGetTickCount();
          uint32_t interval = g_weatherValid ? WEATHER_REFRESH_MS : WEATHER_RETRY_MS;

          if ((now - lastWeatherTick) >= pdMS_TO_TICKS(interval))
          {
            lastWeatherTick = now;
            Weather_UpdateOnce();
          }
#endif
          osDelay(1000);
        }
      }

      g_wifiConnected = 0;
      g_wifiState = WIFI_STATE_FAILED;
    }
    else
    {
      (void)ret;
      g_wifiConnected = 0;
      g_wifiState = WIFI_STATE_FAILED;

#if NTP_ENABLED
      g_ntpSynced = 0;
      g_ntpState = NTP_STATE_FAILED;
#endif
    }

    osDelay(WIFI_RETRY_DELAY_MS);
  }
#else
  g_wifiState = WIFI_STATE_DISABLED;
  g_wifiConnected = 0;
  g_ntpState = NTP_STATE_DISABLED;
  g_ntpSynced = 0;
  for (;;)
  {
    osDelay(10000);
  }
#endif
}
