#pragma once

#include <WiFi.h>
#include <NetworkClient.h>
#include <NetworkServer.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <string.h>

// USB-only diagnostics. LAN serves synthetic bytes on the AP address for
// 90 seconds after the explicit LAN command. It exposes no configuration.
namespace router_diagnostics {

static NetworkServer lanServer(IPAddress(192, 168, 4, 1), 8080, 1);
static bool lanActive = false;
static uint32_t lanStarted = 0;
static char command[32];
static size_t commandLength = 0;
static bool commandOverflow = false;
static const uint8_t synthetic[1460] = {0};

inline bool retryable() {
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

inline bool sendBounded(int fd, const uint8_t *data, size_t length,
                        uint32_t started, uint32_t limitMs) {
  size_t offset = 0;
  while (offset < length && millis() - started < limitMs) {
    const int sent = lwip_send(fd, data + offset, length - offset, MSG_DONTWAIT);
    if (sent > 0) offset += sent;
    else if (sent < 0 && retryable()) delay(1);
    else return false;
  }
  return offset == length;
}

inline void status() {
  wifi_ap_record_t uplink = {};
  wifi_bandwidth_t staBw = WIFI_BW_HT20, apBw = WIFI_BW_HT20;
  wifi_ps_type_t ps = WIFI_PS_NONE;
  const esp_err_t staResult = esp_wifi_sta_get_ap_info(&uplink);
  const esp_err_t staBwResult = esp_wifi_get_bandwidth(WIFI_IF_STA, &staBw);
  const esp_err_t apBwResult = esp_wifi_get_bandwidth(WIFI_IF_AP, &apBw);
  const esp_err_t psResult = esp_wifi_get_ps(&ps);
  Serial.printf("[DIAG] STA connected=%d IP=%s BSSID=%02X:%02X:%02X:%02X:%02X:%02X RSSI=%d channel=%u secondary=%d bw=%d bw_result=%d ps=%d ps_result=%d heap=%u\n",
                staResult == ESP_OK, WiFi.localIP().toString().c_str(),
                uplink.bssid[0], uplink.bssid[1], uplink.bssid[2],
                uplink.bssid[3], uplink.bssid[4], uplink.bssid[5],
                uplink.rssi, uplink.primary, uplink.second,
                static_cast<int>(staBw), static_cast<int>(staBwResult),
                static_cast<int>(ps), static_cast<int>(psResult), ESP.getFreeHeap());
  wifi_sta_list_t clients = {};
  const esp_err_t clientResult = esp_wifi_ap_get_sta_list(&clients);
  Serial.printf("[DIAG] AP IP=%s clients=%d bw=%d bw_result=%d list_result=%d\n",
                WiFi.softAPIP().toString().c_str(), clients.num,
                static_cast<int>(apBw), static_cast<int>(apBwResult),
                static_cast<int>(clientResult));
  if (clientResult == ESP_OK) {
    for (int i = 0; i < clients.num; ++i) {
      const wifi_sta_info_t &s = clients.sta[i];
      Serial.printf("[DIAG] AP client=%02X:%02X:%02X:%02X:%02X:%02X RSSI=%d 11b=%u 11g=%u 11n=%u\n",
                    s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4], s.mac[5],
                    s.rssi, s.phy_11b, s.phy_11g, s.phy_11n);
    }
  }
}

inline void wan() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WAN] not connected");
    return;
  }
  status();
  NetworkClient client;
  const uint32_t started = millis();
  if (!client.connect(IPAddress(121, 29, 30, 68), 80, 5000)) {
    Serial.printf("[WAN] connect failed elapsed_ms=%lu\n", (unsigned long)(millis() - started));
    return;
  }
  const uint32_t connectMs = millis() - started;
  static const char request[] =
      "GET /ubuntu/ls-lR.gz HTTP/1.0\r\n"
      "Host: mirrors.aliyun.com\r\n"
      "Range: bytes=0-4999999\r\n"
      "Accept-Encoding: identity\r\n"
      "Connection: close\r\n\r\n";
  const int fd = client.fd();
  if (!sendBounded(fd, reinterpret_cast<const uint8_t *>(request), sizeof(request) - 1,
                   started, 20000)) {
    Serial.println("[WAN] request send failed");
    client.stop();
    return;
  }
  uint8_t buffer[1460];
  char headers[2048];
  size_t headerLength = 0, bytes = 0;
  bool headerComplete = false;
  int httpCode = 0;
  uint32_t bodyStarted = 0;
  const char *reason = "deadline";
  while (millis() - started < 20000 && bytes < 5000000UL) {
    const int received = lwip_recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
    if (received < 0) {
      if (retryable()) { delay(1); continue; }
      reason = "receive_error";
      break;
    }
    if (received == 0) { reason = "closed"; break; }
    size_t offset = 0;
    while (!headerComplete && offset < static_cast<size_t>(received)) {
      if (headerLength + 1 >= sizeof(headers)) { reason = "headers_too_large"; break; }
      headers[headerLength++] = static_cast<char>(buffer[offset++]);
      headers[headerLength] = '\0';
      if (headerLength >= 4 && memcmp(headers + headerLength - 4, "\r\n\r\n", 4) == 0) {
        headerComplete = true;
        sscanf(headers, "HTTP/%*s %d", &httpCode);
        bodyStarted = millis();
        // Fixed-length response only, to avoid counting chunk framing as data.
        if (strcasestr(headers, "Transfer-Encoding: chunked") != nullptr) reason = "chunked_unsupported";
        else if (httpCode != 200 && httpCode != 206) reason = "http_error";
      }
    }
    if (!headerComplete && headerLength + 1 >= sizeof(headers)) break;
    if (strcmp(reason, "chunked_unsupported") == 0 || strcmp(reason, "http_error") == 0) break;
    if (headerComplete) bytes += static_cast<size_t>(received) - offset;
    delay(1);
  }
  const uint32_t elapsed = millis() - started;
  const uint32_t bodyMs = headerComplete ? millis() - bodyStarted : 0;
  if (bytes >= 5000000UL) reason = "byte_limit";
  client.stop();
  Serial.printf("[WAN] HTTP=%d bytes=%u elapsed_ms=%lu connect_ms=%lu body_ms=%lu total_Mbps=%.3f body_Mbps=%.3f end=%s\n",
                httpCode, (unsigned)bytes, (unsigned long)elapsed,
                (unsigned long)connectMs, (unsigned long)bodyMs,
                elapsed ? bytes * 8.0 / (elapsed * 1000.0) : 0.0,
                bodyMs ? bytes * 8.0 / (bodyMs * 1000.0) : 0.0, reason);
}

inline void lanStart() {
  if (lanActive) lanServer.end();
  lanServer.begin();
  lanActive = static_cast<bool>(lanServer);
  lanStarted = millis();
  Serial.printf("[LAN] enabled=%d URL=http://192.168.4.1:8080/download bytes=1048576 lifetime_s=90\n", lanActive);
}

inline void lanPoll() {
  if (!lanActive) return;
  if (millis() - lanStarted >= 90000UL) {
    lanServer.end();
    lanActive = false;
    Serial.println("[LAN] server expired");
    return;
  }
  NetworkClient client = lanServer.accept();
  if (!client) return;
  // Accept is nonblocking. A single client's operation is bounded to 20 s;
  // all socket I/O is nonblocking and yields to the Wi-Fi/lwIP tasks.
  client.setNoDelay(true);
  const int fd = client.fd();
  const uint32_t started = millis();
  char request[1024];
  size_t used = 0;
  bool complete = false;
  while (millis() - started < 2000 && millis() - lanStarted < 90000UL && used + 1 < sizeof(request)) {
    const int count = lwip_recv(fd, request + used, sizeof(request) - used - 1, MSG_DONTWAIT);
    if (count < 0) {
      if (retryable()) { delay(1); continue; }
      break;
    }
    if (!count) break;
    used += count;
    request[used] = '\0';
    if (strstr(request, "\r\n\r\n")) { complete = true; break; }
  }
  const bool valid = complete &&
      (strncmp(request, "GET /download HTTP/", 19) == 0 || strncmp(request, "GET / HTTP/", 11) == 0);
  if (!valid) {
    static const char bad[] = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    sendBounded(fd, reinterpret_cast<const uint8_t *>(bad), sizeof(bad) - 1, started, 3000);
    client.stop();
    return;
  }
  static const char response[] =
      "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n"
      "Content-Length: 1048576\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
  if (!sendBounded(fd, reinterpret_cast<const uint8_t *>(response), sizeof(response) - 1, started, 20000)) {
    client.stop();
    return;
  }
  size_t sent = 0;
  const uint32_t bodyStarted = millis();
  while (sent < 1048576UL && millis() - started < 20000 && millis() - lanStarted < 90000UL) {
    const size_t remaining = 1048576UL - sent;
    const size_t size = remaining < sizeof(synthetic) ? remaining : sizeof(synthetic);
    const int count = lwip_send(fd, synthetic, size, MSG_DONTWAIT);
    if (count > 0) sent += count;
    else if (count < 0 && retryable()) delay(1);
    else break;
  }
  const uint32_t elapsed = millis() - bodyStarted;
  client.stop();
  // Sender timing measures bytes queued; curl on the client is authoritative.
  Serial.printf("[LAN] queued_bytes=%u elapsed_ms=%lu client_measurement_required=1\n",
                (unsigned)sent, (unsigned long)elapsed);
}

inline void commandReady() {
  command[commandLength] = '\0';
  if (!commandOverflow && commandLength) {
    if (strcmp(command, "STATUS") == 0) status();
    else if (strcmp(command, "WAN") == 0) wan();
    else if (strcmp(command, "LAN") == 0) lanStart();
    else Serial.println("[DIAG] commands: STATUS WAN LAN");
  }
  commandLength = 0;
  commandOverflow = false;
}

}  // namespace router_diagnostics

inline void diagnosticsPoll() {
  using namespace router_diagnostics;
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n') commandReady();
    else if (c != '\r') {
      if (commandLength + 1 < sizeof(command)) command[commandLength++] = c;
      else commandOverflow = true;
    }
  }
  lanPoll();
}
