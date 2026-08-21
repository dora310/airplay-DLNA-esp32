#include "web_api_v3.h"

#include "audio/software_dsp.h"
#include "cJSON.h"
#include "dlna_renderer.h"
#include "esp_app_desc.h"
#include "playback_control.h"
#include "settings.h"
#include "source_manager.h"
#include "system_monitor.h"
#include "wifi.h"
#include <stdlib.h>
#include <string.h>

static esp_err_t send_json(httpd_req_t *req, cJSON *json) {
  char *body = cJSON_PrintUnformatted(json);
  if (!body)
    return ESP_ERR_NO_MEM;
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t err = httpd_resp_sendstr(req, body);
  free(body);
  return err;
}

static cJSON *read_json(httpd_req_t *req) {
  if (req->content_len <= 0 || req->content_len > 4096)
    return NULL;
  char *body = calloc(1, req->content_len + 1);
  if (!body)
    return NULL;
  size_t offset = 0;
  while (offset < req->content_len) {
    int n = httpd_req_recv(req, body + offset, req->content_len - offset);
    if (n <= 0) {
      free(body);
      return NULL;
    }
    offset += (size_t)n;
  }
  cJSON *json = cJSON_Parse(body);
  free(body);
  return json;
}

static bool authorized(httpd_req_t *req) {
  if (!settings_web_password_is_set())
    return true;
  size_t len = httpd_req_get_hdr_value_len(req, "X-API-Key");
  if (!len || len > 64)
    return false;
  char key[65];
  if (httpd_req_get_hdr_value_str(req, "X-API-Key", key, sizeof(key)) != ESP_OK)
    return false;
  return settings_verify_web_password(key);
}

static esp_err_t require_auth(httpd_req_t *req) {
  if (authorized(req))
    return ESP_OK;
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req,
                     "{\"success\":false,\"error\":\"X-API-Key required\"}");
  return ESP_ERR_INVALID_STATE;
}

static esp_err_t status_get(httpd_req_t *req) {
  system_health_t h;
  system_monitor_get(&h);
  cJSON *j = cJSON_CreateObject();
  cJSON_AddBoolToObject(j, "success", true);
  cJSON_AddStringToObject(j, "version", esp_app_get_description()->version);
  cJSON_AddStringToObject(j, "source",
                          source_manager_name(source_manager_current()));
  cJSON_AddBoolToObject(j, "wifi", wifi_is_connected());
  cJSON_AddNumberToObject(j, "volume", playback_control_get_volume_percent());
  cJSON_AddBoolToObject(j, "muted", playback_control_is_muted());
  cJSON_AddNumberToObject(j, "uptime_seconds", h.uptime_seconds);
  cJSON_AddNumberToObject(j, "free_heap", h.free_heap);
  cJSON_AddBoolToObject(j, "password_set", settings_web_password_is_set());
  esp_err_t err = send_json(req, j);
  cJSON_Delete(j);
  return err;
}

static esp_err_t health_get(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK)
    return ESP_OK;
  system_health_t h;
  system_monitor_get(&h);
  cJSON *j = cJSON_CreateObject();
  cJSON_AddBoolToObject(j, "success", true);
  cJSON_AddNumberToObject(j, "uptime_seconds", h.uptime_seconds);
  cJSON_AddNumberToObject(j, "free_heap", h.free_heap);
  cJSON_AddNumberToObject(j, "minimum_free_heap", h.minimum_free_heap);
  cJSON_AddNumberToObject(j, "largest_internal_block",
                          h.largest_internal_block);
  cJSON_AddNumberToObject(j, "free_psram", h.free_psram);
  cJSON_AddNumberToObject(j, "restart_count", h.restart_count);
  cJSON_AddNumberToObject(j, "reset_reason", h.reset_reason);
  cJSON_AddNumberToObject(j, "low_memory_events", h.low_memory_events);
  cJSON_AddNumberToObject(j, "source_switches", source_manager_switch_count());
  cJSON_AddNumberToObject(j, "limiter_events", software_dsp_limiter_count());
  esp_err_t err = send_json(req, j);
  cJSON_Delete(j);
  return err;
}

static esp_err_t control_post(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK)
    return ESP_OK;
  cJSON *in = read_json(req);
  cJSON *action = in ? cJSON_GetObjectItem(in, "action") : NULL;
  bool ok = action && cJSON_IsString(action);
  if (ok) {
    const char *a = action->valuestring;
    if (!strcmp(a, "play_pause"))
      playback_control_play_pause();
    else if (!strcmp(a, "next"))
      playback_control_next();
    else if (!strcmp(a, "previous"))
      playback_control_prev();
    else if (!strcmp(a, "volume_up"))
      playback_control_volume_up();
    else if (!strcmp(a, "volume_down"))
      playback_control_volume_down();
    else if (!strcmp(a, "mute"))
      playback_control_toggle_mute();
    else
      ok = false;
  }
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", ok);
  if (!ok)
    cJSON_AddStringToObject(out, "error", "Unknown action");
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  cJSON_Delete(in);
  return err;
}

static void dsp_to_json(cJSON *j, const software_dsp_config_t *c) {
  cJSON_AddBoolToObject(j, "enabled", c->enabled);
  cJSON_AddBoolToObject(j, "limiter", c->limiter_enabled);
  cJSON_AddBoolToObject(j, "normalization", c->normalization_enabled);
  cJSON_AddNumberToObject(j, "normalization_target_dbfs",
                          c->normalization_target_dbfs);
  cJSON_AddNumberToObject(j, "balance", c->balance);
  cJSON_AddNumberToObject(j, "channel", c->channel);
  cJSON_AddNumberToObject(j, "crossover", c->crossover);
  cJSON_AddNumberToObject(j, "crossover_hz", c->crossover_hz);
  cJSON *bands = cJSON_AddArrayToObject(j, "bands");
  for (int i = 0; i < SOFTWARE_DSP_PEAK_BANDS; i++) {
    cJSON *b = cJSON_CreateObject();
    cJSON_AddNumberToObject(b, "frequency_hz", c->bands[i].frequency_hz);
    cJSON_AddNumberToObject(b, "gain_db", c->bands[i].gain_db);
    cJSON_AddNumberToObject(b, "q", c->bands[i].q);
    cJSON_AddItemToArray(bands, b);
  }
}

static esp_err_t dsp_get(httpd_req_t *req) {
  software_dsp_config_t cfg;
  software_dsp_get_config(&cfg);
  cJSON *j = cJSON_CreateObject();
  cJSON_AddBoolToObject(j, "success", true);
  dsp_to_json(j, &cfg);
  esp_err_t err = send_json(req, j);
  cJSON_Delete(j);
  return err;
}

static esp_err_t dsp_post(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK)
    return ESP_OK;
  cJSON *j = read_json(req);
  if (!j)
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
  software_dsp_config_t c;
  software_dsp_get_config(&c);
#define READ_BOOL(name, field)               \
  do {                                       \
    cJSON *v = cJSON_GetObjectItem(j, name); \
    if (cJSON_IsBool(v))                     \
      c.field = cJSON_IsTrue(v);             \
  } while (0)
#define READ_NUM(name, field)                \
  do {                                       \
    cJSON *v = cJSON_GetObjectItem(j, name); \
    if (cJSON_IsNumber(v))                   \
      c.field = (float)v->valuedouble;       \
  } while (0)
  READ_BOOL("enabled", enabled);
  READ_BOOL("limiter", limiter_enabled);
  READ_BOOL("normalization", normalization_enabled);
  READ_NUM("normalization_target_dbfs", normalization_target_dbfs);
  READ_NUM("balance", balance);
  READ_NUM("crossover_hz", crossover_hz);
  cJSON *v = cJSON_GetObjectItem(j, "channel");
  if (cJSON_IsNumber(v))
    c.channel = (software_dsp_channel_t)v->valueint;
  v = cJSON_GetObjectItem(j, "crossover");
  if (cJSON_IsNumber(v))
    c.crossover = (software_dsp_crossover_t)v->valueint;
  cJSON *bands = cJSON_GetObjectItem(j, "bands");
  if (cJSON_IsArray(bands))
    for (int i = 0; i < SOFTWARE_DSP_PEAK_BANDS; i++) {
      cJSON *b = cJSON_GetArrayItem(bands, i);
      if (!cJSON_IsObject(b))
        continue;
      v = cJSON_GetObjectItem(b, "frequency_hz");
      if (cJSON_IsNumber(v))
        c.bands[i].frequency_hz = (float)v->valuedouble;
      v = cJSON_GetObjectItem(b, "gain_db");
      if (cJSON_IsNumber(v))
        c.bands[i].gain_db = (float)v->valuedouble;
      v = cJSON_GetObjectItem(b, "q");
      if (cJSON_IsNumber(v))
        c.bands[i].q = (float)v->valuedouble;
    }
  software_dsp_set_config(&c);
  cJSON_Delete(j);
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", true);
  dsp_to_json(out, &c);
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  return err;
}

static esp_err_t radio_post(httpd_req_t *req) {
  if (require_auth(req) != ESP_OK)
    return ESP_OK;
  cJSON *j = read_json(req);
  cJSON *u = j ? cJSON_GetObjectItem(j, "url") : NULL;
  esp_err_t result = ESP_ERR_INVALID_ARG;
  if (cJSON_IsString(u))
    result = dlna_renderer_play_uri(u->valuestring);
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", result == ESP_OK);
  if (result != ESP_OK)
    cJSON_AddStringToObject(out, "error", esp_err_to_name(result));
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  cJSON_Delete(j);
  return err;
}

static esp_err_t password_post(httpd_req_t *req) {
  if (settings_web_password_is_set() && require_auth(req) != ESP_OK)
    return ESP_OK;
  cJSON *j = read_json(req);
  cJSON *p = j ? cJSON_GetObjectItem(j, "password") : NULL;
  esp_err_t result = cJSON_IsString(p)
                         ? settings_set_web_password(p->valuestring)
                         : ESP_ERR_INVALID_ARG;
  cJSON *out = cJSON_CreateObject();
  cJSON_AddBoolToObject(out, "success", result == ESP_OK);
  if (result != ESP_OK)
    cJSON_AddStringToObject(out, "error",
                            "Password must be empty or 8-64 characters");
  esp_err_t err = send_json(req, out);
  cJSON_Delete(out);
  cJSON_Delete(j);
  return err;
}

esp_err_t web_api_v3_register(httpd_handle_t s) {
  const httpd_uri_t routes[] = {
      {.uri = "/api/v1/status", .method = HTTP_GET, .handler = status_get},
      {.uri = "/api/v1/health", .method = HTTP_GET, .handler = health_get},
      {.uri = "/api/v1/control", .method = HTTP_POST, .handler = control_post},
      {.uri = "/api/v1/dsp", .method = HTTP_GET, .handler = dsp_get},
      {.uri = "/api/v1/dsp", .method = HTTP_POST, .handler = dsp_post},
      {.uri = "/api/v1/radio", .method = HTTP_POST, .handler = radio_post},
      {.uri = "/api/v1/security/password",
       .method = HTTP_POST,
       .handler = password_post},
  };
  for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
    esp_err_t err = httpd_register_uri_handler(s, &routes[i]);
    if (err != ESP_OK)
      return err;
  }
  return ESP_OK;
}
