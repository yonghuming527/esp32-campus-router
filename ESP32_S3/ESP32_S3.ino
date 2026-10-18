/*
 * JRXY Dr.COM adaptation of moqin3380/esp32-campus-router.
 * Upstream source: ../upstream/ESP32_S3.ino.txt
 * Retains AP/STA coexistence, open-network connection, diagnostics and NAPT.
 * Authentication follows this campus's served Dr.COM 4.2.1 JavaScript.
 */
#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_arduino_version.h"
#include "esp_netif_net_stack.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/lwip_napt.h"
#include "config_private.h"
#if ESP_ARDUINO_VERSION_MAJOR < 3
#error "Arduino ESP32 Core 3.x or later is required for NAPT"
#endif

String jget(const String &j,const String &k){
  String kk="\""+k+"\""; int p=j.indexOf(kk); if(p<0)return "";
  p=j.indexOf(':',p+kk.length()); if(p<0)return ""; p++;
  while(p<(int)j.length()&&(j[p]==' '||j[p]=='"'))p++;
  int e=p; while(e<(int)j.length()&&j[e]!='"'&&j[e]!=','&&j[e]!='}')e++;
  return j.substring(p,e);
}
/* ---------- URL 编码 (application/x-www-form-urlencoded) ---------- */
String urlEnc(const String &s){
  const char *hex="0123456789ABCDEF";
  String o; o.reserve(s.length()+16);
  for(size_t i=0;i<s.length();i++){
    unsigned char c=(unsigned char)s[i];
    if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||
       c=='-'||c=='_'||c=='.'||c=='~'||c=='*'){
      o+=(char)c;
    }else{
      o+='%'; o+=hex[c>>4]; o+=hex[c&15];
    }
  }
  return o;
}

#include "DrcomAuth.h"
#include "RouterDiagnostics.h"

// Windows' live connection identifies the campus SSID with a hyphen.
// Prefer the requested name when present; use the observed spelling only
// if it is discovered as an open 2.4 GHz network during startup.
static String campusSSID = CAMPUS_SSID;

static const char* discReason(uint8_t r){
  switch(r){
    case WIFI_REASON_NO_AP_FOUND:          return "找不到该SSID (名字错/超出范围/是5G频段)";
    case WIFI_REASON_AUTH_FAIL:            return "认证失败 (密码错/需要802.1x企业认证)";
    case WIFI_REASON_ASSOC_FAIL:           return "关联失败 (AP拒绝/信号太弱)";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:    return "握手超时 (密码错或信号差)";
    case WIFI_REASON_CONNECTION_FAIL:      return "连接失败";
    case WIFI_REASON_BEACON_TIMEOUT:       return "信标丢失 (AP关机/太远)";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:return "四次握手超时 (密码错)";
    case WIFI_REASON_AUTH_EXPIRE:          return "认证过期";
    case WIFI_REASON_802_1X_AUTH_FAILED:   return "802.1x 企业认证失败";
    default:                               return "未知原因";
  }
}
/* 扫描一次, 确认目标 SSID 到底在不在 2.4G 列表里 */
static void scanCheck(){
  Serial.println("[SCAN] 开始扫描 2.4G AP ...");
  int n = WiFi.scanNetworks();
  if(n <= 0){ Serial.println("[SCAN] 一个都没扫到! 天线/供电/射频问题"); return; }
  bool found = false;
  bool observedAliasFound = false;
  for(int i=0;i<n;i++){
    String s = WiFi.SSID(i);
    Serial.printf("  %2d) ch=%-3ld rssi=%-4ld %s%s\n",
      i+1, (long)WiFi.channel(i), (long)WiFi.RSSI(i),
      s.c_str(),
      (s==String(CAMPUS_SSID)) ? "   <<< 目标!" : "");
    if(s==String(CAMPUS_SSID)) found = true;
    if(s=="JRXY-Student" && WiFi.encryptionType(i)==WIFI_AUTH_OPEN)
      observedAliasFound = true;
  }
  if(!found && observedAliasFound){
    campusSSID = "JRXY-Student";
    found = true;
    Serial.println("[SCAN] Using observed campus SSID: JRXY-Student");
  }
  if(found) Serial.println("[SCAN] 目标 SSID 在线, 问题在密码/认证方式");
  else      Serial.println("[SCAN] 目标 SSID 不在列表! 检查名字拼写或改用2.4G");
  WiFi.scanDelete();
}
/* 统一连接入口: 开放网络(空密码)要用单参版本, 否则可能关联失败 */
static void staConnect(){
  if(String(CAMPUS_PASS).length() == 0){
    WiFi.begin(campusSSID.c_str());               /* 开放/无密码 */
  }else{
    WiFi.begin(campusSSID.c_str(), CAMPUS_PASS);
  }
}

static volatile bool gotIP = false;
static volatile unsigned long lastStaDisconnect = 0;
static volatile uint32_t staDisconnects = 0;
static volatile uint32_t apDisconnects = 0;
static bool apStarted = false;
static bool authenticated = false;
static bool routerReady = false;
static unsigned long lastRouterTry = 0;
static unsigned long lastAuth = 0;
static unsigned long lastConnect = 0;
static unsigned long lastStatus = 0;
static unsigned long lastRouterCheck = 0;

bool naptActive() {
  LOCK_TCPIP_CORE();
  esp_netif_t *handle = WiFi.AP.netif();
  struct netif *ap = handle ? static_cast<struct netif *>(esp_netif_get_netif_impl(handle)) : nullptr;
  const bool active = ap && netif_is_up(ap) && netif_is_link_up(ap) && ap->napt;
  UNLOCK_TCPIP_CORE();
  return active;
}

void printRouteState() {
  char rows[6][176];
  size_t count = 0;
  LOCK_TCPIP_CORE();
  for (struct netif *n = netif_list; n && count < 6; n = n->next) {
    char ip[16], mask[16], gw[16];
    ip4addr_ntoa_r(netif_ip4_addr(n), ip, sizeof(ip));
    ip4addr_ntoa_r(netif_ip4_netmask(n), mask, sizeof(mask));
    ip4addr_ntoa_r(netif_ip4_gw(n), gw, sizeof(gw));
    snprintf(rows[count++], sizeof(rows[0]), "[ROUTE] %c%c%u ip=%s mask=%s gw=%s up=%d link=%d napt=%u default=%d\n",
      n->name[0], n->name[1], n->num, ip, mask, gw,
      netif_is_up(n), netif_is_link_up(n), n->napt, n == netif_default);
  }
  UNLOCK_TCPIP_CORE();
  // USB backpressure must never hold the TCP/IP stack lock.
  for (size_t i = 0; i < count; ++i) Serial.print(rows[i]);
}

void onEvt(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    gotIP = true;
  } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    gotIP = false;
    lastStaDisconnect = millis();
    ++staDisconnects;
    Serial.printf("[STA] disconnected: %u (%s) uptime_ms=%lu count=%lu\n",
                  info.wifi_sta_disconnected.reason,
                  discReason(info.wifi_sta_disconnected.reason),
                  (unsigned long)millis(), (unsigned long)staDisconnects);
  } else if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
    Serial.println("[AP] client connected");
  } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
    ++apDisconnects;
    Serial.printf("[AP] client disconnected uptime_ms=%lu count=%lu\n",
                  (unsigned long)millis(), (unsigned long)apDisconnects);
  }
}

bool startRouter() {
  if (!apStarted) {
    apStarted = WiFi.softAP(AP_SSID, AP_PASS, WiFi.channel(), false, 4);
    if (!apStarted) {
      Serial.println("[AP] start failed");
      return false;
    }
    // Applying the SSID restarts the AP asynchronously. netif_add() clears
    // NAPT during that restart; wait before enabling forwarding, as in the
    // upstream project's delayed naptTask.
    WiFi.softAPbandwidth(WIFI_BW_HT20);
    WiFi.setSleep(false);
    delay(750);
  }
  IPAddress apIP(192, 168, 4, 1), mask(255, 255, 255, 0);
  IPAddress lease(192, 168, 4, 2);
  // Verified from a hotspot client: the supplied 111.11.1.1 times out,
  // while the campus backup 111.11.11.1 answers correctly through NAPT.
  IPAddress dns;
  if (!dns.fromString(AP_DNS)) dns = WiFi.dnsIP(0);
  if (!WiFi.softAPConfig(apIP, apIP, mask, lease, dns)) {
    Serial.println("[AP] DHCP/DNS config failed");
    return false;
  }
  esp_netif_t *ap = WiFi.AP.netif();
  if (!ap) return false;
  esp_netif_t *sta = WiFi.STA.netif();
  if (!sta || esp_netif_set_default_netif(sta) != ESP_OK) return false;
  esp_err_t err = esp_netif_napt_enable(ap);
  Serial.printf("[NAPT] %s\n", esp_err_to_name(err));
  // Verify the actual flag instead of trusting only an API return code.
  bool active = false;
  err_t fallback = ERR_OK;
  bool usedFallback = false;
  LOCK_TCPIP_CORE();
  struct netif *apNetif = static_cast<struct netif *>(esp_netif_get_netif_impl(ap));
  if (apNetif && !apNetif->napt) {
    usedFallback = true;
    fallback = ip_napt_enable_netif(apNetif, 1);
  }
  active = apNetif && apNetif->napt;
  UNLOCK_TCPIP_CORE();
  if (usedFallback) Serial.printf("[NAPT] lwIP fallback result=%d\n", fallback);
  Serial.printf("[NAPT] verified active=%d\n", active);
  Serial.printf("[AP] SSID=%s IP=%s DNS=%s channel=%d\n",
                AP_SSID, WiFi.softAPIP().toString().c_str(),
                dns.toString().c_str(), WiFi.channel());
  printRouteState();
  return active;
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(5);
  delay(1500);
  Serial.println("\n=== JRXY ESP32-S3 Dr.COM Campus Router ===");
  Serial.printf("[BOOT] reset_reason=%d uptime_ms=%lu\n", (int)esp_reset_reason(), (unsigned long)millis());
  Serial.printf("[BUILD] %s %s; core=%d.%d.%d\n", __DATE__, __TIME__,
                ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR,
                ESP_ARDUINO_VERSION_PATCH);
  Serial.printf("[CHIP] flash=%u MB PSRAM=%u KB\n",
                ESP.getFlashChipSize() / 1048576, ESP.getPsramSize() / 1024);
  WiFi.persistent(false);
  WiFi.onEvent(onEvt);
  // Preserve this SDK's RX=8 default instead of Arduino's reduced RX=4.
  // The qio_opi SDK uses dynamic TX; this API also restores its TX defaults.
  WiFi.useStaticBuffers(true);
  WiFi.mode(WIFI_STA);
  Serial.printf("[WIFI] sdk_buffers=1 rx=%d ba_window=%d tx_type=%d dynamic_tx=%d\n",
                CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM, CONFIG_ESP_WIFI_RX_BA_WIN,
                CONFIG_ESP_WIFI_TX_BUFFER_TYPE, CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  // Survey all matching APs before choosing the strongest signal.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.STA.bandwidth(WIFI_BW_HT20);
  scanCheck();
  staConnect();
  lastConnect = millis();
  Serial.printf("[STA] connecting to %s\n", campusSSID.c_str());
}

void loop() {
  diagnosticsPoll();
  if (WiFi.status() == WL_CONNECTED) {
    if (millis() - lastRouterCheck >= 1000UL) {
      lastRouterCheck = millis();
      if (!naptActive()) routerReady = false;
    }
    if (gotIP) {
      gotIP = false;
      lastConnect = millis();
      authenticated = false;
      Serial.printf("[STA] IP=%s MAC=%s RSSI=%d BSSID=%s channel=%d\n",
                    WiFi.localIP().toString().c_str(),
                    WiFi.macAddress().c_str(), WiFi.RSSI(),
                    WiFi.BSSIDstr().c_str(), WiFi.channel());
      lastRouterTry = millis();
      routerReady = startRouter();
      // HTTP work runs here, never in the WiFi event callback.
      lastAuth = millis();
      authenticated = drcomLogin();
      if (authenticated) internetCheck();
    } else if (millis() - lastAuth >= 120000UL) {
      lastAuth = millis();
      authenticated = drcomLogin();
      if (authenticated) internetCheck();
    }
    if (!routerReady && millis() - lastRouterTry >= 10000UL) {
      lastRouterTry = millis();
      routerReady = startRouter();
    }
  } else {
    authenticated = false;
    routerReady = false;
    // Let Arduino finish auto-reconnection and DHCP. reconnect() would
    // forcibly disconnect an already associated station waiting for an IP.
    if (millis() - lastConnect >= 30000UL &&
        millis() - lastStaDisconnect >= 30000UL) {
      lastConnect = millis();
      wifi_ap_record_t associatedAP = {};
      if (esp_wifi_sta_get_ap_info(&associatedAP) != ESP_OK) {
        Serial.println("[STA] reconnect fallback after 30s without association");
        WiFi.reconnect();
      }
    }
  }
  if (millis() - lastStatus >= 30000UL) {
    lastStatus = millis();
    Serial.printf("[STATUS] wifi=%s auth=%s router=%s clients=%u freeHeap=%u rssi=%d\n",
                  WiFi.status() == WL_CONNECTED ? "up" : "down",
                  authenticated ? "ok" : "pending",
                  routerReady ? "ok" : "pending",
                  WiFi.softAPgetStationNum(), ESP.getFreeHeap(), WiFi.RSSI());
    printRouteState();
    Serial.printf("[HEALTH] uptime_ms=%lu reset_reason=%d sta_drops=%lu ap_leaves=%lu min_heap=%u largest_internal=%u\n",
                  (unsigned long)millis(), (int)esp_reset_reason(),
                  (unsigned long)staDisconnects, (unsigned long)apDisconnects,
                  ESP.getMinFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  delay(50);
}

