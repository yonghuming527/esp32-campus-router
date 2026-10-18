#pragma once

#include <WiFi.h>
#include <HTTPClient.h>
#include <ctype.h>

// Matches this campus portal's a41.js and a40.js (jsVersion 4.2.1):
// login_method=1, account_prefix=0, en_md5=0. Use the ESP32's own DHCP
// address and station MAC; never reuse the browser screenshot's identity.
namespace drcom_detail {

inline String encode(const String &value) {
  static const char hex[] = "0123456789ABCDEF";
  String out;
  out.reserve(value.length() * 3);
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

// The endpoints return a JSON object wrapped in JSONP. Read a named scalar
// without accepting a substring such as an error message containing "ok".
inline String value(const String &body, const char *name) {
  const String key = String('"') + name + '"';
  int at = body.indexOf(key);
  if (at < 0) return "";
  at += key.length();
  while (at < static_cast<int>(body.length()) && isspace(static_cast<unsigned char>(body[at]))) ++at;
  if (at >= static_cast<int>(body.length()) || body[at++] != ':') return "";
  while (at < static_cast<int>(body.length()) && isspace(static_cast<unsigned char>(body[at]))) ++at;
  if (at >= static_cast<int>(body.length())) return "";
  if (body[at] == '"') {
    ++at;
    String out;
    bool escaped = false;
    for (; at < static_cast<int>(body.length()); ++at) {
      const char c = body[at];
      if (escaped) {
        // Identity/result fields contain ASCII; preserve unknown escapes for
        // diagnostics rather than pretending to decode arbitrary JSON text.
        if (c == '"' || c == '\\' || c == '/') out += c;
        else { out += '\\'; out += c; }
        escaped = false;
      } else if (c == '\\') escaped = true;
      else if (c == '"') return out;
      else out += c;
    }
    return "";  // Truncated JSON string.
  }
  const int start = at;
  while (at < static_cast<int>(body.length()) && body[at] != ',' &&
         body[at] != '}' && !isspace(static_cast<unsigned char>(body[at]))) ++at;
  return body.substring(start, at);
}

inline bool request(const String &url, const char *operation, String &body) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!http.begin(url)) {
    Serial.printf("[DRCOM] %s: could not initialize HTTP\n", operation);
    return false;
  }
  http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
  http.addHeader("Referer", String(PORTAL) + "/");
  const int code = http.GET();
  const int size = http.getSize();
  // Never print the request URL: the login query contains the password.
  if (code != HTTP_CODE_OK || size > 8192) {
    Serial.printf("[DRCOM] %s: HTTP %d, response bytes %d\n", operation, code, size);
    http.end();
    return false;
  }
  body = http.getString();
  http.end();
  if (body.length() == 0 || body.length() > 8192) {
    Serial.printf("[DRCOM] %s: empty/oversized response\n", operation);
    return false;
  }
  return true;
}

inline String querySuffix() {
  return "&jsVersion=4.2.1&terminal_type=1&lang=zh&v=" + String(millis());
}

enum class Status { Online, Offline, Unknown };

inline Status status() {
  String body;
  if (!request(String(PORTAL) + "/drcom/chkstatus?callback=esp32" + querySuffix(), "status", body))
    return Status::Unknown;
  const String result = value(body, "result");
  if (result == "0") return Status::Offline;
  if (result != "1") return Status::Unknown;
  String ip = value(body, "v4ip");
  if (ip.isEmpty()) ip = value(body, "v46ip");
  if (ip.isEmpty()) ip = value(body, "ss5");
  const String account = value(body, "uid");
  if (ip == WiFi.localIP().toString() && account == String(USERNAME))
    return Status::Online;
  Serial.println("[DRCOM] Online response identity did not match this ESP32/account");
  return Status::Unknown;
}

inline String redacted(String text) {
  // Print only selected error fields, and redact credentials if the server
  // happens to echo them there, including their URL-encoded representation.
  const String pass = PASSWORD;
  const String account = USERNAME;
  if (!pass.isEmpty()) { text.replace(pass, "***"); text.replace(encode(pass), "***"); }
  if (!account.isEmpty()) {
    text.replace(account, "***");
    text.replace(encode(account), "***");
    const int suffix = account.indexOf('@');
    if (suffix > 0) text.replace(account.substring(0, suffix), "***");
  }
  text.replace("\r", " ");
  text.replace("\n", " ");
  if (text.length() > 240) text = text.substring(0, 240);
  return text;
}

}  // namespace drcom_detail

inline bool drcomLogin() {
  using namespace drcom_detail;
  if (WiFi.status() != WL_CONNECTED) return false;
  const Status current = status();
  if (current == Status::Online) {
    Serial.println("[DRCOM] Already online; no login request sent");
    return true;
  }
  if (current == Status::Unknown) {
    Serial.println("[DRCOM] Status could not be verified; retry on next scheduled check");
    return false;
  }

  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.replace("-", "");
  mac.toUpperCase();
  String url = String(DRCOM_API) + "/eportal/portal/login?callback=esp32&login_method=1";
  url += "&user_account=" + encode(USERNAME);
  url += "&user_password=" + encode(PASSWORD);
  url += "&wlan_user_ip=" + encode(WiFi.localIP().toString());
  url += "&wlan_user_ipv6=&wlan_user_mac=" + encode(mac);
  url += "&wlan_ac_ip=" + encode(DRCOM_AC_IP) + "&wlan_ac_name=";
  url += querySuffix();
  String body;
  if (!request(url, "login", body)) return false;
  const String result = value(body, "result");
  if (result == "1" || result == "ok") {
    Serial.println("[DRCOM] LOGIN OK (portal result confirmed)");
    return true;
  }
  // ret_code 2 means this IP is online; verify ownership with the status
  // endpoint instead of accepting the code as success unconditionally.
  if (value(body, "ret_code") == "2" && status() == Status::Online) {
    Serial.println("[DRCOM] Online identity confirmed");
    return true;
  }
  Serial.printf("[DRCOM] LOGIN FAILED result=%s ret_code=%s msg=%s\n",
                redacted(result).c_str(), redacted(value(body, "ret_code")).c_str(),
                redacted(value(body, "msg")).c_str());
  return false;
}

inline bool internetCheck() {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  if (!http.begin("http://www.msftconnecttest.com/connecttest.txt")) return false;
  const int code = http.GET();
  String body;
  if (code == HTTP_CODE_OK && http.getSize() <= 256) body = http.getString();
  http.end();
  body.trim();
  const bool ok = code == HTTP_CODE_OK && body == "Microsoft Connect Test";
  Serial.printf("[INTERNET] %s (HTTP %d)\n", ok ? "OK" : "not confirmed", code);
  return ok;
}
