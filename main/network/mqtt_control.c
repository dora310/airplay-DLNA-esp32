#include "mqtt_control.h"

#ifdef CONFIG_MQTT_CONTROL_ENABLE
#include "esp_log.h"
#include "mqtt_client.h"
#include "playback_control.h"
#include "settings.h"
#include <string.h>

static const char *TAG = "mqtt";
static esp_mqtt_client_handle_t s_client;
static char s_prefix[96];

static void publish_discovery(void) {
  char name[65], topic[160], payload[640];
  settings_get_device_name(name, sizeof(name));
  snprintf(topic, sizeof(topic),
           "homeassistant/media_player/esp32_airplay/config");
  snprintf(payload, sizeof(payload),
           "{\"name\":\"%s\",\"unique_id\":\"esp32_airplay\","
           "\"command_topic\":\"%s/command\",\"state_topic\":\"%s/state\","
           "\"availability_topic\":\"%s/"
           "availability\",\"payload_available\":\"online\","
           "\"payload_not_available\":\"offline\"}",
           name, s_prefix, s_prefix, s_prefix);
  esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 1);
  snprintf(topic, sizeof(topic), "%s/availability", s_prefix);
  esp_mqtt_client_publish(s_client, topic, "online", 0, 1, 1);
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id,
                       void *data) {
  (void)arg;
  (void)base;
  esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;
  if (id == MQTT_EVENT_CONNECTED) {
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/command", s_prefix);
    esp_mqtt_client_subscribe(s_client, topic, 1);
    publish_discovery();
  } else if (id == MQTT_EVENT_DATA) {
    if (e->data_len == 10 && !strncmp(e->data, "play_pause", 10))
      playback_control_play_pause();
    else if (e->data_len == 4 && !strncmp(e->data, "next", 4))
      playback_control_next();
    else if (e->data_len == 8 && !strncmp(e->data, "previous", 8))
      playback_control_prev();
    else if (e->data_len == 6 && !strncmp(e->data, "vol_up", 6))
      playback_control_volume_up();
    else if (e->data_len == 8 && !strncmp(e->data, "vol_down", 8))
      playback_control_volume_down();
    else if (e->data_len == 4 && !strncmp(e->data, "mute", 4))
      playback_control_toggle_mute();
  }
}

esp_err_t mqtt_control_start(void) {
  strlcpy(s_prefix, CONFIG_MQTT_TOPIC_PREFIX, sizeof(s_prefix));
  esp_mqtt_client_config_t cfg = {.broker.address.uri = CONFIG_MQTT_BROKER_URI};
  s_client = esp_mqtt_client_init(&cfg);
  if (!s_client)
    return ESP_ERR_NO_MEM;
  esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
  ESP_LOGI(TAG, "Starting MQTT/Home Assistant integration");
  return esp_mqtt_client_start(s_client);
}
#else
esp_err_t mqtt_control_start(void) {
  return ESP_ERR_NOT_SUPPORTED;
}
#endif
