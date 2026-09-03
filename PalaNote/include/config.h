#ifndef CONFIG_H
#define CONFIG_H

#define EPD_SPI_NUM        SPI2_HOST
#define ESP32_I2C_DEV_NUM  I2C_NUM_0

#define EPD_WIDTH  200
#define EPD_HEIGHT 200
#define LVGL_SPIRAM_BUFF_LEN (EPD_WIDTH * EPD_HEIGHT * 2)

/* EPD SPI pins */
#define EPD_DC_PIN    GPIO_NUM_10
#define EPD_CS_PIN    GPIO_NUM_11
#define EPD_SCK_PIN   GPIO_NUM_12
#define EPD_MOSI_PIN  GPIO_NUM_13
#define EPD_RST_PIN   GPIO_NUM_9
#define EPD_BUSY_PIN  GPIO_NUM_8


/* Power control pins */
#define EPD_PWR_PIN     GPIO_NUM_6
#define Audio_PWR_PIN   GPIO_NUM_42
#define VBAT_PWR_PIN    GPIO_NUM_17
#define BAT_ADC_PIN     4   // ADC1_CHANNEL_3 on ESP32-S3

#define BOOT_BUTTON_PIN GPIO_NUM_0
#define PWR_BUTTON_PIN  GPIO_NUM_18

/* Deep-sleep wake-up pin */
#define ext_wakeup_pin_1 GPIO_NUM_0

/* I2C bus */
#define ESP32_I2C_SDA_PIN GPIO_NUM_47
#define ESP32_I2C_SCL_PIN GPIO_NUM_48

/* LVGL tick timing */
#define EXAMPLE_LVGL_TICK_PERIOD_MS    5
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 500
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 100

/* I2C peripheral addresses */
#define I2C_RTC_DEV_Address        0x51
#define I2C_SHTC3_DEV_Address      0x70

/* Button GPIO aliases */
#define BTN_REC      0
#define BTN_PWR      18
#define PWR_HOLD_PIN 17

/* SD-MMC pins */
#define SD_CLK  39
#define SD_CMD  41
#define SD_D0   40

/* Audio */
#define SAMPLE_RATE  16000
#define REC_BUF      (8 * 1024)

/* Storage paths */
#define NOTES_DIR  "/notes"
#define INDEX_FILE "/notes/index.csv"
#define TAG_FILE   "/notes/tags.txt"
#define MAX_TAGS   20

/* UI timing */
#define REC_HOLD_MS         350
#define BTN_LONG_MS         600
#define DOUBLE_MS           200
#define ULTRA_SLEEP_MS      120000UL
#define TICKER_INTERVAL_MS  950

/* Battery warning */
#define BAT_CHECK_INTERVAL_MS  30000
#define BAT_LOW_THRESHOLD      15
#define BAT_RECOVER_THRESHOLD  20

/* Notes server (ai-meeting-buddy). Recordings go here first: the audio and the
   Whisper transcription both stay on your own server. Uploads are chunked and
   resumable - the server is the only source of truth for how many bytes it
   already has, so a dropped connection or a reboot resumes instead of
   restarting. OpenAI is only a fallback if the server can't be reached.
   The host, port and scheme all come from UPLOAD_SERVER_URL in secrets.h. */
/* 256 KB per chunk. Each chunk is a fresh TLS connection here (unlike the
   M5Stick client this protocol came from, which keeps one alive across a
   whole file), so a small chunk size means many handshakes: at 8 KB a 2 MB
   note was 256 of them, which buried the server's request queue and timed
   the device out mid-upload. The chunk is streamed from SD in a 1 KB
   buffer, so a bigger chunk costs no more RAM. */
#define UPLOAD_CHUNK_BYTES  262144UL  // bytes per POST /upload/chunk request
#define UPLOAD_HTTP_TO_MS   20000UL   // per-request response timeout
/* Measured from the last byte that actually moved, not from the start of
   the request, so a slow-but-progressing link is never cut off - only one
   that has genuinely stopped. */
#define UPLOAD_STALL_TO_MS  15000UL   // no progress this long mid-chunk = dead link
#define UPLOAD_MAX_RETRIES  3         // per-chunk retries before giving up on this note

/* Time & firmware */
#define LOCAL_TIME_OFFSET_MIN  120   // UTC+2 (Germany summer). Set to your offset.
#define FIRMWARE_VERSION       "v1.1"
#define FW_VERSION             "v1.1"

#endif // CONFIG_H