/*
BME280からデータを取得しAWSioTにアップロードするプログラムです
*/
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <cJSON.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "aws_iot_config.h"
#include "aws_iot_log.h"
#include "aws_iot_version.h"
#include "aws_iot_mqtt_client_interface.h"

//BME280
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "bme280.h"
#include "sdkconfig.h"

static const char *TAG = "subpub";

//sdkconfig内で設定されたWIFIが設定される
#define EXAMPLE_WIFI_SSID CONFIG_WIFI_SSID
#define EXAMPLE_WIFI_PASS CONFIG_WIFI_PASSWORD

//define for BME280
#define SDA_PIN GPIO_NUM_21
#define SCL_PIN GPIO_NUM_22
#define TAG_BME280 "BME280"
#define I2C_MASTER_ACK 0
#define I2C_MASTER_NACK 1

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
but we only care about one event - are we connected
to the AP with an IP? */
const int CONNECTED_BIT = BIT0;


/*
AWSから落としてきた証明書が設定されます
*/
#if defined(CONFIG_EXAMPLE_EMBEDDED_CERTS)

extern const uint8_t aws_root_ca_pem_start[] asm("_binary_aws_root_ca_pem_start");
extern const uint8_t aws_root_ca_pem_end[] asm("_binary_aws_root_ca_pem_end");
extern const uint8_t certificate_pem_crt_start[] asm("_binary_certificate_pem_crt_start");
extern const uint8_t certificate_pem_crt_end[] asm("_binary_certificate_pem_crt_end");
extern const uint8_t private_pem_key_start[] asm("_binary_private_pem_key_start");
extern const uint8_t private_pem_key_end[] asm("_binary_private_pem_key_end");

#elif defined(CONFIG_EXAMPLE_FILESYSTEM_CERTS)

static const char * DEVICE_CERTIFICATE_PATH = CONFIG_EXAMPLE_CERTIFICATE_PATH;
static const char * DEVICE_PRIVATE_KEY_PATH = CONFIG_EXAMPLE_PRIVATE_KEY_PATH;
static const char * ROOT_CA_PATH = CONFIG_EXAMPLE_ROOT_CA_PATH;

#else
#error "Invalid method for loading certs"
#endif

/**
* @brief Default MQTT HOST URL is pulled from the aws_iot_config.h
*/
char HostAddress[255] = AWS_IOT_MQTT_HOST;

/**
* @brief Default MQTT port is pulled from the aws_iot_config.h
*/
uint32_t port = AWS_IOT_MQTT_PORT;


static esp_err_t event_handler(void *ctx, system_event_t *event)
{
  switch(event->event_id) {
    case SYSTEM_EVENT_STA_START:
    esp_wifi_connect();
    break;
    case SYSTEM_EVENT_STA_GOT_IP:
    xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
    break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
    /* This is a workaround as ESP32 WiFi libs don't currently
    auto-reassociate. */
    esp_wifi_connect();
    xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
    break;
    default:
    break;
  }
  return ESP_OK;
}

void iot_subscribe_callback_handler(AWS_IoT_Client *pClient, char *topicName, uint16_t topicNameLen,
  IoT_Publish_Message_Params *params, void *pData) {
    ESP_LOGI(TAG, "Subscribe callback");
    ESP_LOGI(TAG, "%.*s\t%.*s", topicNameLen, topicName, (int) params->payloadLen, (char *)params->payload);
  }

  void disconnectCallbackHandler(AWS_IoT_Client *pClient, void *data) {
    ESP_LOGW(TAG, "MQTT Disconnect");
    IoT_Error_t rc = FAILURE;

    if(NULL == pClient) {
      return;
    }

    if(aws_iot_is_autoreconnect_enabled(pClient)) {
      ESP_LOGI(TAG, "Auto Reconnect is enabled, Reconnecting attempt will start now");
    } else {
      ESP_LOGW(TAG, "Auto Reconnect not enabled. Starting manual reconnect...");
      rc = aws_iot_mqtt_attempt_reconnect(pClient);
      if(NETWORK_RECONNECTED == rc) {
        ESP_LOGW(TAG, "Manual Reconnect Successful");
      } else {
        ESP_LOGW(TAG, "Manual Reconnect Failed - %d", rc);
      }
    }
  }

  static void initialise_wifi(void)
  {
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    wifi_config_t wifi_config = {
      .sta = {
        .ssid = EXAMPLE_WIFI_SSID,
        .password = EXAMPLE_WIFI_PASS,
      },
    };
    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
  }

  void i2c_master_init()
  {
    i2c_config_t i2c_config = {
      .mode = I2C_MODE_MASTER,
      .sda_io_num = SDA_PIN,
      .scl_io_num = SCL_PIN,
      .sda_pullup_en = GPIO_PULLUP_ENABLE,
      .scl_pullup_en = GPIO_PULLUP_ENABLE,
      .master.clk_speed = 1000000
    };
    i2c_param_config(I2C_NUM_0, &i2c_config);
    i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
  }

  s8 BME280_I2C_bus_write(u8 dev_addr, u8 reg_addr, u8 *reg_data, u8 cnt)
  {
    s32 iError = BME280_INIT_VALUE;

    esp_err_t espRc;
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);

    i2c_master_write_byte(cmd, reg_addr, true);
    i2c_master_write(cmd, reg_data, cnt, true);
    i2c_master_stop(cmd);

    espRc = i2c_master_cmd_begin(I2C_NUM_0, cmd, 10/portTICK_PERIOD_MS);
    if (espRc == ESP_OK) {
      iError = SUCCESS;
    } else {
      iError = FAIL;
    }
    i2c_cmd_link_delete(cmd);

    return (s8)iError;
  }

  s8 BME280_I2C_bus_read(u8 dev_addr, u8 reg_addr, u8 *reg_data, u8 cnt)
  {
    s32 iError = BME280_INIT_VALUE;
    esp_err_t espRc;

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg_addr, true);

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_READ, true);

    if (cnt > 1) {
      i2c_master_read(cmd, reg_data, cnt-1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, reg_data+cnt-1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    espRc = i2c_master_cmd_begin(I2C_NUM_0, cmd, 1000/portTICK_PERIOD_MS);
    if (espRc == ESP_OK) {
      iError = SUCCESS;
    } else {
      iError = FAIL;
    }

    i2c_cmd_link_delete(cmd);

    return (s8)iError;
  }

  void BME280_delay_msek(u32 msek)
  {
    vTaskDelay(msek/portTICK_PERIOD_MS);
  }


//ここからメインの関数
  void aws_iot_task(void *param) {
    char cPayload[100];

    IoT_Error_t rc = FAILURE;

    AWS_IoT_Client client;
    IoT_Client_Init_Params mqttInitParams = iotClientInitParamsDefault;
    IoT_Client_Connect_Params connectParams = iotClientConnectParamsDefault;

    IoT_Publish_Message_Params paramsQOS0;
    IoT_Publish_Message_Params paramsQOS1;

    ESP_LOGI(TAG, "AWS IoT SDK Version %d.%d.%d-%s", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH, VERSION_TAG);

    mqttInitParams.enableAutoReconnect = false; // We enable this later below
    mqttInitParams.pHostURL = HostAddress;
    mqttInitParams.port = port;

    #if defined(CONFIG_EXAMPLE_EMBEDDED_CERTS)
    mqttInitParams.pRootCALocation = (const char *)aws_root_ca_pem_start;
    mqttInitParams.pDeviceCertLocation = (const char *)certificate_pem_crt_start;
    mqttInitParams.pDevicePrivateKeyLocation = (const char *)private_pem_key_start;

    #elif defined(CONFIG_EXAMPLE_FILESYSTEM_CERTS)
    mqttInitParams.pRootCALocation = ROOT_CA_PATH;
    mqttInitParams.pDeviceCertLocation = DEVICE_CERTIFICATE_PATH;
    mqttInitParams.pDevicePrivateKeyLocation = DEVICE_PRIVATE_KEY_PATH;
    #endif

    mqttInitParams.mqttCommandTimeout_ms = 20000;
    mqttInitParams.tlsHandshakeTimeout_ms = 5000;
    mqttInitParams.isSSLHostnameVerify = true;
    mqttInitParams.disconnectHandler = disconnectCallbackHandler;
    mqttInitParams.disconnectHandlerData = NULL;

    #ifdef CONFIG_EXAMPLE_SDCARD_CERTS
    ESP_LOGI(TAG, "Mounting SD card...");
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 3,
    };
    sdmmc_card_t* card;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to mount SD card VFAT filesystem. Error: %s", esp_err_to_name(ret));
      abort();
    }
    #endif

    rc = aws_iot_mqtt_init(&client, &mqttInitParams);
    if(SUCCESS != rc) {
      ESP_LOGE(TAG, "aws_iot_mqtt_init returned error : %d ", rc);
      abort();
    }

    /* Wait for WiFI to show as connected */
    xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
      false, true, portMAX_DELAY);

      connectParams.keepAliveIntervalInSec = 10;
      connectParams.isCleanSession = true;
      connectParams.MQTTVersion = MQTT_3_1_1;
      /* Client ID is set in the menuconfig of the example */
      connectParams.pClientID = CONFIG_AWS_EXAMPLE_CLIENT_ID;
      connectParams.clientIDLen = (uint16_t) strlen(CONFIG_AWS_EXAMPLE_CLIENT_ID);
      connectParams.isWillMsgPresent = false;

      ESP_LOGI(TAG, "Connecting to AWS...");
      do {
        rc = aws_iot_mqtt_connect(&client, &connectParams);
        if(SUCCESS != rc) {
          ESP_LOGE(TAG, "Error(%d) connecting to %s:%d", rc, mqttInitParams.pHostURL, mqttInitParams.port);
          vTaskDelay(1000 / portTICK_RATE_MS);
        }
      } while(SUCCESS != rc);

      /*
      * Enable Auto Reconnect functionality. Minimum and Maximum time of Exponential backoff are set in aws_iot_config.h
      *  #AWS_IOT_MQTT_MIN_RECONNECT_WAIT_INTERVAL
      *  #AWS_IOT_MQTT_MAX_RECONNECT_WAIT_INTERVAL
      */
      rc = aws_iot_mqtt_autoreconnect_set_status(&client, true);
      if(SUCCESS != rc) {
        ESP_LOGE(TAG, "Unable to set Auto Reconnect to true - %d", rc);
        abort();
      }

      struct bme280_t bme280 = {
        .bus_write = BME280_I2C_bus_write,
        .bus_read = BME280_I2C_bus_read,
        .dev_addr = BME280_I2C_ADDRESS2,
        .delay_msec = BME280_delay_msek
      };

      s32 com_rslt;
      s32 v_uncomp_pressure_s32;
      s32 v_uncomp_temperature_s32;
      s32 v_uncomp_humidity_s32;
      //BME280の設定を行います
      com_rslt = bme280_init(&bme280);

      com_rslt += bme280_set_oversamp_pressure(BME280_OVERSAMP_16X);
      com_rslt += bme280_set_oversamp_temperature(BME280_OVERSAMP_16X);
      com_rslt += bme280_set_oversamp_humidity(BME280_OVERSAMP_16X);
      com_rslt += bme280_set_standby_durn(BME280_STANDBY_TIME_1000_MS);
      com_rslt += bme280_set_filter(BME280_FILTER_COEFF_8);
      com_rslt += bme280_set_power_mode(BME280_NORMAL_MODE);

      //AWS内のトピック設定
      const char *topic = "BME280/data/";
      const int TOPIC_LEN = strlen(topic);

      //subscribing
      // ESP_LOGI(TAG, "Subscribing...");
      // rc = aws_iot_mqtt_subscribe(&client, TOPIC, TOPIC_LEN, QOS0, iot_subscribe_callback_handler, NULL);
      // if(SUCCESS != rc) {
      //     ESP_LOGE(TAG, "Error subscribing : %d ", rc);
      //     abort();
      // }

      //sprintf(cPayload, "%s : %d ", "hello from SDK", i);

      // paramsQOS0.qos = QOS0;
      // paramsQOS0.payload = (void *) cPayload;
      // paramsQOS0.isRetained = 0;

      //define QOS1
      paramsQOS1.qos = QOS1;
      paramsQOS1.payload = (void *) cPayload;
      paramsQOS1.isRetained = 0;
      cJSON *root = NULL;

      while((NETWORK_ATTEMPTING_RECONNECT == rc || NETWORK_RECONNECTED == rc || SUCCESS == rc) && com_rslt == SUCCESS) {

        //Max time the yield function will wait for read messages
        rc = aws_iot_mqtt_yield(&client, 100);
        if(NETWORK_ATTEMPTING_RECONNECT == rc) {
          // If the client is attempting to reconnect we will skip the rest of the loop.
          continue;
        }

        com_rslt = bme280_read_uncomp_pressure_temperature_humidity(
          &v_uncomp_pressure_s32, &v_uncomp_temperature_s32, &v_uncomp_humidity_s32);

          if (com_rslt == SUCCESS) {
            ESP_LOGI(TAG_BME280, "%.2f degC / %.3f hPa / %.3f %%",
            bme280_compensate_temperature_double(v_uncomp_temperature_s32),
            bme280_compensate_pressure_double(v_uncomp_pressure_s32)/100, // Pa -> hPa
            bme280_compensate_humidity_double(v_uncomp_humidity_s32));
          } else {
            ESP_LOGE(TAG_BME280, "measure error. code: %d", com_rslt);
          }

          ESP_LOGI(TAG, "Stack remaining for task '%s' is %d bytes", pcTaskGetTaskName(NULL), uxTaskGetStackHighWaterMark(NULL));
          // vTaskDelay(1000 / portTICK_RATE_MS);

          //QOS0
          // sprintf(cPayload, "%.2f degC / %.3g hPa / %.3f %%",
          // bme280_compensate_temperature_double(v_uncomp_temperature_s32),
          // bme280_compensate_pressure_double(v_uncomp_pressure_s32)/100, // Pa -> hPa
          // bme280_compensate_humidity_double(v_uncomp_humidity_s32));
          // paramsQOS0.payloadLen = strlen(cPayload);
          // rc = aws_iot_mqtt_publish(&client, TOPIC, TOPIC_LEN, &paramsQOS0);


          //sprintf(cPayload, "value : %.2f",
          //bme280_compensate_temperature_double(v_uncomp_temperature_s32)); // Pa -> hPa

          /*
          rootというJSONを作成し、データを格納しパブリッシュしています
          */
          root = cJSON_CreateObject();
          int int_tmp = bme280_compensate_temperature_double(v_uncomp_temperature_s32)*100;
          double double_tmp = int_tmp/100.00;
          cJSON_AddNumberToObject(root, "tempreture", double_tmp);
          int_tmp = bme280_compensate_pressure_double(v_uncomp_pressure_s32)/100*100;
          double_tmp = int_tmp/100.00;
          cJSON_AddNumberToObject(root, "pressure", double_tmp);
          int_tmp = bme280_compensate_humidity_double(v_uncomp_humidity_s32)*100;
          double_tmp = int_tmp/100.00;
          cJSON_AddNumberToObject(root, "humidity", double_tmp);
          sprintf(cPayload,"%s",cJSON_Print(root));
          paramsQOS1.payloadLen = strlen(cPayload);
          rc = aws_iot_mqtt_publish(&client, topic, TOPIC_LEN, &paramsQOS1);
          if (rc == MQTT_REQUEST_TIMEOUT_ERROR) {
              ESP_LOGW(TAG, "QOS1 publish ack not received.");
              rc = SUCCESS;
          }

          ESP_LOGE(TAG_BME280,"%s",cPayload);

          cJSON_Delete(root);

          vTaskDelay(30000/ portTICK_RATE_MS);

        }
        ESP_LOGE(TAG_BME280, "init or setting error. code: %d", com_rslt);
        ESP_LOGE(TAG, "An error occurred in the main loop.");
        abort();
      }


void app_main()
      {
        i2c_master_init();
        //xTaskCreatePinnedToCore(&task_bme280_normal_mode, "bme280_normal_mode",  2048, NULL, 6, NULL,0);
        // Initialize NVS.
        esp_err_t err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
          ESP_ERROR_CHECK(nvs_flash_erase());
          err = nvs_flash_init();
        }
        ESP_ERROR_CHECK( err );

        initialise_wifi();
        xTaskCreatePinnedToCore(&aws_iot_task, "aws_iot_task", 9216, NULL, 5, NULL, 1);
}
