#include "rtsp_server.h"

#include <errno.h>
#include <ctype.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "lwip/inet.h"
#include "mdns.h"

#include "audio_receiver.h"
#include "audio_output.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "rtsp_conn.h"
#include "rtsp_crypto.h"
#include "rtsp_handlers.h"
#include "rtsp_message.h"

#include "ntp_clock.h"
#include "ptp_clock.h"
#include "rtsp_events.h"
#include "dacp_client.h"
#include "display.h"

static const char *TAG = "rtsp_server";

#define RTSP_PORT           7000
#define RTSP_BUFFER_INITIAL 4096
#define RTSP_BUFFER_LARGE   ((size_t)256 * 1024)

#define CLIENT_STACK_SIZE 8192
#define SERVER_STACK_SIZE 4096

static int server_socket = -1;
static TaskHandle_t server_task_handle = NULL;
static bool server_running = false;

// RTSP tasks are restartable. Use dynamic TCB allocation so
// reconnect/start-stop paths cannot reuse static task memory before FreeRTOS
// idle finishes deletion.

// Client slot for tracking connections
typedef struct {
  rtsp_conn_t *conn;
  TaskHandle_t task;
  int socket;
  volatile bool should_stop;
  volatile bool is_old; // Marked as old client being killed
} client_slot_t;

static client_slot_t clients[2] = {0}; // Current and old
static int current_slot = 0;

// Flag set by the play/pause button to tell the grace period loop
// to send a DACP resume command and keep waiting for reconnect.
static volatile bool s_resume_requested = false;
// Incremented for every accepted AirPlay client.  A slow DNS lookup from an
// old session must never overwrite the name of a newer sender.
static volatile uint32_t s_sender_generation = 0;
static volatile uint32_t s_sender_explicit_generation = 0;

typedef struct {
  uint32_t client_ip;
  uint32_t generation;
} sender_lookup_args_t;

static void format_sender_fallback(uint32_t client_ip, const char *kind,
                                   char *out, size_t out_size) {
  struct in_addr address = {.s_addr = client_ip};
  char ip[INET_ADDRSTRLEN] = {0};
  if (!inet_ntop(AF_INET, &address, ip, sizeof(ip))) {
    strlcpy(ip, "unknown IP", sizeof(ip));
  }
  snprintf(out, out_size, "%.*s \xE2\x80\xA2 %s", 55,
           kind ? kind : "AirPlay sender", ip);
}

static void clean_sender_name(char *name) {
  if (!name) {
    return;
  }

  // Remove surrounding quotes and control characters received in headers.
  char *start = name;
  while (*start && (isspace((unsigned char)*start) || *start == '"')) {
    start++;
  }
  if (start != name) {
    memmove(name, start, strlen(start) + 1);
  }
  for (char *p = name; *p; p++) {
    if ((unsigned char)*p < 0x20) {
      *p = ' ';
    }
  }
  size_t len = strlen(name);
  while (len > 0 && (isspace((unsigned char)name[len - 1]) ||
                     name[len - 1] == '"')) {
    name[--len] = '\0';
  }

  // Local DNS commonly returns "Qas-iPhone.local".  The suffix adds no
  // useful information on the display.
  if (len > 6 && strcasecmp(name + len - 6, ".local") == 0) {
    name[len - 6] = '\0';
  }
}

static bool name_looks_private_token(const char *name) {
  if (!name || strlen(name) < 11) {
    return false;
  }
  for (const char *p = name; *p; p++) {
    if (!isxdigit((unsigned char)*p) && *p != '-' && *p != '_') {
      return false;
    }
  }
  return true;
}

static bool mdns_result_matches_ip(const mdns_result_t *result,
                                   uint32_t client_ip) {
  if (!result) {
    return false;
  }
  for (const mdns_ip_addr_t *address = result->addr; address;
       address = address->next) {
    if (address->addr.type == ESP_IPADDR_TYPE_V4 &&
        address->addr.u_addr.ip4.addr == client_ip) {
      return true;
    }
  }
  return false;
}

static void sender_lookup_task(void *context) {
  sender_lookup_args_t *args = context;
  if (!args) {
    vTaskDelete(NULL);
    return;
  }

  // These potentially blocking Bonjour queries deliberately run at low
  // priority and never inside the RTSP receive or audio tasks. Apple devices
  // commonly advertise _companion-link; classic AirPlay senders may instead
  // expose a _dacp hostname.
  char hostname[96] = {0};
  static const struct {
    const char *service;
    bool prefer_instance;
  } queries[] = {{"_companion-link", true}, {"_dacp", false}};

  for (size_t query = 0;
       query < sizeof(queries) / sizeof(queries[0]) && hostname[0] == '\0';
       query++) {
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr(queries[query].service, "_tcp", 1200, 16,
                                   &results);
    if (err != ESP_OK || !results) {
      continue;
    }
    for (mdns_result_t *item = results; item; item = item->next) {
      if (!mdns_result_matches_ip(item, args->client_ip)) {
        continue;
      }
      const char *candidate =
          queries[query].prefer_instance ? item->instance_name : item->hostname;
      if ((!candidate || candidate[0] == '\0') && item->hostname) {
        candidate = item->hostname;
      }
      if (candidate && !name_looks_private_token(candidate)) {
        strlcpy(hostname, candidate, sizeof(hostname));
      }
      break;
    }
    mdns_query_results_free(results);
  }
  clean_sender_name(hostname);

  if (hostname[0] != '\0' && args->generation == s_sender_generation &&
      args->generation != s_sender_explicit_generation) {
    ESP_LOGI(TAG, "AirPlay sender resolved: %s", hostname);
    display_notify_airplay_sender(hostname);
  }

  free(args);
  vTaskDelete(NULL);
}

static void sender_identity_connected(uint32_t client_ip) {
  uint32_t generation = ++s_sender_generation;
  s_sender_explicit_generation = 0;
  char fallback[96];
  format_sender_fallback(client_ip, "AirPlay sender", fallback,
                         sizeof(fallback));
  display_notify_airplay_sender(fallback);

  sender_lookup_args_t *args = calloc(1, sizeof(*args));
  if (!args) {
    return;
  }
  args->client_ip = client_ip;
  args->generation = generation;
  if (xTaskCreate(sender_lookup_task, "sender_name", 3072, args, 1, NULL) !=
      pdPASS) {
    free(args);
  }
}

static bool copy_header_value(const char *headers, const char *header_name,
                              char *out, size_t out_size) {
  if (!headers || !header_name || !out || out_size == 0) {
    return false;
  }

  size_t name_len = strlen(header_name);
  const char *line = headers;
  while (*line) {
    const char *line_end = strstr(line, "\r\n");
    if (!line_end) {
      line_end = line + strlen(line);
    }
    if ((size_t)(line_end - line) > name_len &&
        strncasecmp(line, header_name, name_len) == 0 &&
        line[name_len] == ':') {
      const char *value = line + name_len + 1;
      while (value < line_end && isspace((unsigned char)*value)) {
        value++;
      }
      size_t value_len = (size_t)(line_end - value);
      if (value_len >= out_size) {
        value_len = out_size - 1;
      }
      memcpy(out, value, value_len);
      out[value_len] = '\0';
      clean_sender_name(out);
      return out[0] != '\0';
    }
    if (*line_end == '\0') {
      break;
    }
    line = line_end + 2;
  }
  return false;
}

static void sender_identity_observe_headers(const rtsp_conn_t *conn,
                                            const char *headers) {
  if (!conn || !headers) {
    return;
  }

  // These friendly-name headers are optional.  Some Apple and third-party
  // senders provide one of them; modern iOS is allowed to omit all of them.
  static const char *const friendly_headers[] = {
      "X-Apple-Client-Name", "X-Apple-Device-Name", "Client-Name"};
  char value[96];
  for (size_t i = 0; i < sizeof(friendly_headers) / sizeof(friendly_headers[0]);
       i++) {
    if (copy_header_value(headers, friendly_headers[i], value,
                          sizeof(value))) {
      ESP_LOGI(TAG, "AirPlay sender name: %s", value);
      s_sender_explicit_generation = s_sender_generation;
      display_notify_airplay_sender(value);
      return;
    }
  }

  // Even without a friendly name, identify the sender type more usefully
  // than a bare address.
  if (s_sender_explicit_generation == s_sender_generation) {
    return;
  }
  if (copy_header_value(headers, "User-Agent", value, sizeof(value))) {
    const char *kind = NULL;
    if (strcasestr(value, "iphone")) {
      kind = "iPhone";
    } else if (strcasestr(value, "ipad")) {
      kind = "iPad";
    } else if (strcasestr(value, "mac") || strcasestr(value, "itunes")) {
      kind = "Mac";
    }
    if (kind) {
      char label[96];
      format_sender_fallback(conn->client_ip, kind, label, sizeof(label));
      display_notify_airplay_sender(label);
    }
  }
}
// PCM5102A has no volume-control register. Keep mute as a non-persistent
// software gate in the final Q15 gain path instead of calling an absent DAC
// driver or overwriting the user's saved volume with -30 dB.
static volatile bool s_output_muted = false;

// Public API for volume control
void airplay_set_volume(float volume_db) {
  client_slot_t *c = &clients[current_slot];
  if (c->conn && !c->is_old) {
    rtsp_conn_set_volume(c->conn, volume_db);
  }
}

void airplay_set_output_muted(bool muted) { s_output_muted = muted; }

bool airplay_output_is_muted(void) { return s_output_muted; }

int32_t airplay_get_volume_q15(void) {
  if (s_output_muted) {
    return 0;
  }
  client_slot_t *c = &clients[current_slot];
  if (c->conn && !c->is_old) {
    return rtsp_conn_get_volume_q15(c->conn);
  }
  return 16384; // 50% volume for new clients
}

void rtsp_server_request_resume(void) {
  s_resume_requested = true;
}

// Helper to grow buffer
static uint8_t *grow_buffer(uint8_t *old_buf, size_t old_size, size_t new_size,
                            size_t data_len) {
  (void)old_size;
  uint8_t *new_buf =
      heap_caps_malloc(new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!new_buf) {
    new_buf = malloc(new_size);
  }
  if (!new_buf) {
    return NULL;
  }
  if (old_buf && data_len > 0) {
    memcpy(new_buf, old_buf, data_len);
  }
  free(old_buf);
  return new_buf;
}

// Process buffered RTSP requests
static void process_rtsp_buffer(client_slot_t *slot, uint8_t *buffer,
                                size_t *buf_len) {
  while (*buf_len > 0 && !slot->should_stop) {
    const uint8_t *header_end = rtsp_find_header_end(buffer, *buf_len);
    if (!header_end) {
      break;
    }

    size_t header_len = (size_t)(header_end - buffer) + 4;
    char *header_str = malloc(header_len + 1);
    if (!header_str) {
      *buf_len = 0;
      break;
    }
    memcpy(header_str, buffer, header_len);
    header_str[header_len] = '\0';

    int content_len = rtsp_parse_content_length(header_str);
    if (content_len < 0) {
      content_len = 0;
    }

    size_t total_len = header_len + (size_t)content_len;
    if (total_len > RTSP_BUFFER_LARGE || *buf_len < total_len) {
      free(header_str);
      if (total_len > RTSP_BUFFER_LARGE) {
        *buf_len = 0;
      }
      break;
    }

    sender_identity_observe_headers(slot->conn, header_str);

    // Null-terminate so strcasestr in parse_raw_header won't read past
    // the message boundary (buffer capacity > total_len).
    uint8_t saved = buffer[total_len];
    buffer[total_len] = '\0';
    rtsp_dispatch(slot->socket, slot->conn, buffer, total_len);
    buffer[total_len] = saved;
    free(header_str);

    if (*buf_len > total_len) {
      memmove(buffer, buffer + total_len, *buf_len - total_len);
    }
    *buf_len -= total_len;
  }
}

// Client task
static void client_task(void *pvParameters) {
  int slot_idx = (int)(intptr_t)pvParameters;
  client_slot_t *slot = &clients[slot_idx];

  // Create connection state
  rtsp_conn_t *conn = rtsp_conn_create();
  if (!conn) {
    ESP_LOGE(TAG, "Failed to create connection state");
    close(slot->socket);
    slot->socket = -1;
    slot->task = NULL;
    vTaskDelete(NULL);
    return;
  }
  slot->conn = conn;

  // Get client IP address for timing requests
  struct sockaddr_in peer_addr;
  socklen_t peer_len = sizeof(peer_addr);
  if (getpeername(slot->socket, (struct sockaddr *)&peer_addr, &peer_len) ==
      0) {
    conn->client_ip = peer_addr.sin_addr.s_addr;
    ESP_LOGI(TAG, "Client IP: %u.%u.%u.%u",
             (unsigned int)(conn->client_ip & 0xFF),
             (unsigned int)((conn->client_ip >> 8) & 0xFF),
             (unsigned int)((conn->client_ip >> 16) & 0xFF),
             (unsigned int)((conn->client_ip >> 24) & 0xFF));
    sender_identity_connected(conn->client_ip);
  }

  // Allocate buffer
  size_t buf_capacity = RTSP_BUFFER_INITIAL;
  uint8_t *buffer = malloc(buf_capacity);
  if (!buffer) {
    ESP_LOGE(TAG, "Failed to allocate buffer");
    rtsp_conn_free(conn);
    slot->conn = NULL;
    close(slot->socket);
    slot->socket = -1;
    slot->task = NULL;
    vTaskDelete(NULL);
    return;
  }

  size_t buf_len = 0;

  // Socket timeout for stop signal responsiveness
  struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(slot->socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  while (server_running && !slot->should_stop) {
    if (conn->encrypted_mode) {
      // Encrypted mode
      while (server_running && conn->encrypted_mode && !slot->should_stop) {
        if (buf_len >= buf_capacity - 1024) {
          size_t new_cap = buf_capacity < RTSP_BUFFER_LARGE ? RTSP_BUFFER_LARGE
                                                            : buf_capacity * 2;
          if (new_cap > RTSP_BUFFER_LARGE) {
            goto cleanup;
          }
          uint8_t *new_buf =
              grow_buffer(buffer, buf_capacity, new_cap, buf_len);
          if (!new_buf) {
            goto cleanup;
          }
          buffer = new_buf;
          buf_capacity = new_cap;
        }

        int block_len = rtsp_crypto_read_block(
            slot->socket, conn, buffer + buf_len, buf_capacity - buf_len);
        if (block_len <= 0) {
          if (slot->should_stop || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            goto cleanup;
          }
          continue;
        }

        buf_len += (size_t)block_len;
        process_rtsp_buffer(slot, buffer, &buf_len);
      }
      goto cleanup;
    }

    // Plain-text mode
    if (buf_len >= buf_capacity - 1024) {
      size_t new_cap = buf_capacity < RTSP_BUFFER_LARGE ? RTSP_BUFFER_LARGE
                                                        : buf_capacity * 2;
      if (new_cap > RTSP_BUFFER_LARGE) {
        break;
      }
      uint8_t *new_buf = grow_buffer(buffer, buf_capacity, new_cap, buf_len);
      if (!new_buf) {
        break;
      }
      buffer = new_buf;
      buf_capacity = new_cap;
    }

    ssize_t recv_len =
        recv(slot->socket, buffer + buf_len, buf_capacity - buf_len, 0);
    if (recv_len <= 0) {
      if (recv_len < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        continue;
      }
      break;
    }
    buf_len += (size_t)recv_len;
    process_rtsp_buffer(slot, buffer, &buf_len);
  }

cleanup:
  ESP_LOGI(TAG, "Client slot %d disconnected", slot_idx);
  free(buffer);
  close(slot->socket);
  slot->socket = -1;

  // Immediate: stop audio and NTP
  audio_receiver_stop();
  audio_output_flush();
  ntp_clock_stop();

  bool has_dacp_remote = conn && conn->protocol_version == 1 &&
                         conn->dacp_id[0] != '\0' &&
                         conn->active_remote[0] != '\0';

  // iOS v1 pause handling needs DACP to distinguish pause from disconnect.
  // Third-party RAOP clients often have no DACP remote; disconnect them
  // immediately instead of delaying slot cleanup with an iOS-only grace path.
  if (has_dacp_remote) {
    if (!slot->should_stop) {
      s_resume_requested = false;
      rtsp_events_emit(RTSP_EVENT_PAUSED, NULL);

      // Phase 1: let mDNS settle (3 s), but exit early on resume or reconnect
      for (int i = 0; i < 6 && !slot->should_stop; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (s_resume_requested) {
          ESP_LOGI(TAG,
                   "Resume requested during Phase 1 — skipping to Phase 2");
          break;
        }
      }

      // Phase 2: wait for reconnect as long as DACP service is advertised.
      // Re-probe every ~5 s. The phone unadvertises the service when the
      // user switches away, so disappearance = genuine disconnect.
      if (!slot->should_stop) {
        bool stay = dacp_probe_service() || s_resume_requested;
        if (s_resume_requested) {
          s_resume_requested = false;
          ESP_LOGI(TAG, "Resume requested via button — waiting for reconnect");
          stay = true;
        }
        if (stay) {
          ESP_LOGI(TAG, "DACP still advertised — waiting for reconnect");
        }
        while (stay && !slot->should_stop) {
          // Wait 5 s between probes (10 × 500 ms), checking flags each tick
          for (int i = 0; i < 10 && !slot->should_stop; i++) {
            vTaskDelay(pdMS_TO_TICKS(500));
            if (s_resume_requested) {
              s_resume_requested = false;
              ESP_LOGI(TAG,
                       "Resume requested via button — extending grace period");
            }
          }
          if (slot->should_stop) {
            break;
          }
          // Re-probe: still advertised?
          stay = dacp_probe_service();
          if (stay) {
            ESP_LOGD(TAG, "DACP still advertised — continuing wait");
          } else {
            ESP_LOGI(TAG, "DACP service gone — genuine disconnect");
          }
        }
      }

      if (slot->is_old) {
        // New client connected during grace period — treat as reconnect
        ESP_LOGI(TAG, "Client reconnected during grace period");
      } else {
        ESP_LOGI(TAG, "Grace period expired — full disconnect");
        dacp_clear_session();
        rtsp_events_emit(RTSP_EVENT_DISCONNECTED, NULL);
      }
    } else {
      // Forcefully stopped (server shutdown or replaced by new client)
      dacp_clear_session();
      rtsp_events_emit(RTSP_EVENT_DISCONNECTED, NULL);
    }
  } else {
    // v2 / unknown — no grace period, clear immediately.
    dacp_clear_session();
    rtsp_events_emit(RTSP_EVENT_DISCONNECTED, NULL);
  }

  // When being replaced by a new client (is_old), skip global state changes —
  // the new session's SETUP already manages PTP and the event port task.
  if (!slot->is_old) {
    ptp_clock_init(); // Restart PTP (stopped during v1 SETUP to free sockets)
    rtsp_stop_event_port_task();
  } else if (rtsp_event_port_listen_socket() >= 0 &&
             rtsp_event_port_listen_socket() == conn->event_socket) {
    // Old task still using our socket — stop it before closing
    rtsp_stop_event_port_task();
  }

  if (conn->event_socket >= 0) {
    close(conn->event_socket);
    conn->event_socket = -1;
  }

  rtsp_conn_cleanup(conn);
  rtsp_conn_free(conn);

  slot->conn = NULL;
  slot->socket = -1;
  slot->task = NULL;
  slot->should_stop = false;
  slot->is_old = false;

  vTaskDelete(NULL);
}

// Signal old client to stop (non-blocking)
static void signal_old_client_stop(int old_slot) {
  client_slot_t *old = &clients[old_slot];
  if (old->task == NULL) {
    return;
  }

  ESP_LOGI(TAG, "Signaling old client to stop");
  old->is_old = true;
  old->should_stop = true;

  // Shutdown socket to unblock recv
  if (old->socket >= 0) {
    shutdown(old->socket, SHUT_RDWR);
  }
  // Task will clean itself up
}

/* The audio receiver, decoder, PTP clock and event port are process-wide
 * resources. A replacement client must not start SETUP while the previous
 * client is still destroying those resources in its cleanup path. Starting
 * both tasks concurrently caused a use-after-free panic during rapid iOS
 * TEARDOWN/reconnect negotiation. */
static bool wait_for_client_cleanup(int slot_idx, uint32_t timeout_ms) {
  TickType_t started = xTaskGetTickCount();
  TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
  while (clients[slot_idx].task != NULL) {
    if ((xTaskGetTickCount() - started) >= timeout) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return true;
}

static void server_task(void *pvParameters) {
  (void)pvParameters;

  struct sockaddr_in server_addr, client_addr;
  socklen_t client_addr_len = sizeof(client_addr);

  // Initialize slots
  for (int i = 0; i < 2; i++) {
    clients[i].socket = -1;
    clients[i].conn = NULL;
    clients[i].task = NULL;
    clients[i].should_stop = false;
    clients[i].is_old = false;
  }

  server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server_socket < 0) {
    ESP_LOGE(TAG, "Failed to create socket: %d", errno);
    server_task_handle = NULL;
    vTaskDelete(NULL);
    return;
  }

  int opt = 1;
  setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  server_addr.sin_port = htons(RTSP_PORT);

  if (bind(server_socket, (struct sockaddr *)&server_addr,
           sizeof(server_addr)) < 0) {
    ESP_LOGE(TAG, "Failed to bind: %d", errno);
    close(server_socket);
    server_socket = -1;
    server_task_handle = NULL;
    vTaskDelete(NULL);
    return;
  }

  if (listen(server_socket, 5) < 0) {
    ESP_LOGE(TAG, "Failed to listen: %d", errno);
    close(server_socket);
    server_socket = -1;
    server_task_handle = NULL;
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(TAG, "RTSP server listening on port %d", RTSP_PORT);
  server_running = true;

  while (server_running) {
    int new_socket = accept(server_socket, (struct sockaddr *)&client_addr,
                            &client_addr_len);
    if (new_socket < 0) {
      if (server_running) {
        ESP_LOGE(TAG, "Failed to accept: %d", errno);
      }
      continue;
    }

    ESP_LOGI(TAG, "New client connected");

    // Find slot for new client (alternate between 0 and 1)
    int new_slot = 1 - current_slot;

    // If new slot still has a running task, wait for it to fully exit.
    // With static TCBs we MUST NOT reuse until the old task is deleted.
    if (clients[new_slot].task != NULL) {
      clients[new_slot].should_stop = true;
      if (clients[new_slot].socket >= 0) {
        shutdown(clients[new_slot].socket, SHUT_RDWR);
      }
      int timeout = 30; // 3 seconds max
      while (clients[new_slot].task != NULL && timeout > 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
        timeout--;
      }
      if (clients[new_slot].task != NULL) {
        ESP_LOGE(TAG, "Slot %d task did not exit in time", new_slot);
        close(new_socket);
        continue;
      }
    }
    /* Stop and JOIN the current client before starting its replacement.
       The previous background cleanup raced the new client's SETUP against
       audio_receiver_stop(), decoder destruction and event-port shutdown. */
    if (clients[current_slot].task != NULL) {
      int old_slot = current_slot;
      signal_old_client_stop(old_slot);
      if (!wait_for_client_cleanup(old_slot, 4000)) {
        ESP_LOGE(TAG,
                 "Old client slot %d did not finish cleanup; rejecting "
                 "replacement safely",
                 old_slot);
        close(new_socket);
        continue;
      }
      ESP_LOGI(TAG, "Old client cleanup complete; starting replacement");
    }

    // Setup new slot
    clients[new_slot].socket = new_socket;
    clients[new_slot].should_stop = false;
    clients[new_slot].is_old = false;

    // Start new client task immediately.
    clients[new_slot].task = NULL;
    BaseType_t task_ret =
        xTaskCreate(client_task, "rtsp_client", CLIENT_STACK_SIZE,
                    (void *)(intptr_t)new_slot, 5, &clients[new_slot].task);
    if (task_ret != pdPASS || clients[new_slot].task == NULL) {
      ESP_LOGE(TAG, "Failed to create client task");
      close(new_socket);
      clients[new_slot].socket = -1;
    } else {
      current_slot = new_slot;
    }
  }

  // Stop all clients
  for (int i = 0; i < 2; i++) {
    if (clients[i].task != NULL) {
      clients[i].should_stop = true;
      if (clients[i].socket >= 0) {
        shutdown(clients[i].socket, SHUT_RDWR);
      }
    }
  }

  vTaskDelay(pdMS_TO_TICKS(500));

  if (server_socket >= 0) {
    close(server_socket);
    server_socket = -1;
  }

  server_task_handle = NULL;
  vTaskDelete(NULL);
}

static bool rtsp_server_wait_for_task_stopped(int timeout_ticks) {
  while (server_task_handle != NULL && timeout_ticks-- > 0) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return server_task_handle == NULL;
}

esp_err_t rtsp_server_start(void) {
  if (server_task_handle != NULL) {
    if (server_running) {
      return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "RTSP server task still stopping, waiting");
    if (!rtsp_server_wait_for_task_stopped(40)) {
      ESP_LOGE(TAG, "Previous RTSP server task did not stop");
      return ESP_ERR_INVALID_STATE;
    }
  }

  BaseType_t task_ret =
      xTaskCreate(server_task, "rtsp_server", SERVER_STACK_SIZE, NULL, 5,
                  &server_task_handle);
  if (task_ret != pdPASS || server_task_handle == NULL) {
    return ESP_FAIL;
  }

  return ESP_OK;
}

void rtsp_server_stop(void) {
  server_running = false;

  if (server_socket >= 0) {
    shutdown(server_socket, SHUT_RDWR);
    close(server_socket);
    server_socket = -1;
  }

  if (server_task_handle != NULL) {
    if (!rtsp_server_wait_for_task_stopped(40)) {
      ESP_LOGW(TAG, "RTSP server task did not exit within timeout");
    }
  }
}
