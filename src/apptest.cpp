// apptest.cpp - drive the TI-84 AI firmware from a phone/computer instead of a calculator.
//
// What it does: joins the WiFi you already saved in the firmware, then serves a web page
// (open the address printed in the serial monitor) with the same actions the calculator has:
// Snap & Solve, Ask, Reply, Scroll Up/Down. It calls the REAL camera.h and openai_client.h
// from your project, so a failure here is a failure in the real code.
//
// The response paging (setResponse / wrap / window / scroll) is COPIED from main.cpp because
// main.cpp keeps it private. If you change that logic in main.cpp, change it here too.
//
// Use: save as src\apptest.cpp, and in platformio.ini set
//   build_src_filter = -<*> +<apptest.cpp> +<wifi_manager.cpp>
// (if the build says WifiManager:: is undefined, add the .cpp file in src that defines it).
// Switch back to  +<main.cpp>  when done. Not part of the calculator firmware.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <algorithm>

#include "camera.h"
#include "config.h"
#include "openai_client.h"
#include "wifi_manager.h"

namespace {

constexpr int kMaxResponseLines = 160;    // same as main.cpp
constexpr int kResponseWindowLines = 5;   // same as main.cpp

WebServer server(80);

int currentTopLine = 0;
int totalResponseLines = 0;
String responseLines[kMaxResponseLines];
String lastRaw = "";
unsigned long lastReconnectTry = 0;

// ---------- copied from main.cpp (paging) ----------

const String &blankResponseLine() {
    static const String line = []() {
        String value;
        value.reserve(SCREEN_WIDTH);
        for (int i = 0; i < SCREEN_WIDTH; i++) value += ' ';
        return value;
    }();
    return line;
}

String padDisplayLine(String value) {
    if (value.length() > SCREEN_WIDTH) value = value.substring(0, SCREEN_WIDTH);
    while (value.length() < SCREEN_WIDTH) value += ' ';
    return value;
}

void clearResponseLines() {
    for (String &line : responseLines) line = blankResponseLine();
    totalResponseLines = 0;
    currentTopLine = 0;
}

void addResponseLine(const String &rawLine) {
    if (totalResponseLines >= kMaxResponseLines) return;
    responseLines[totalResponseLines++] = padDisplayLine(rawLine);
}

void wrapResponseParagraph(String paragraph) {
    if (totalResponseLines >= kMaxResponseLines) return;

    paragraph.replace('\t', ' ');
    paragraph.trim();
    if (paragraph.length() == 0) {
        addResponseLine("");
        return;
    }

    String line = "";
    String word = "";

    auto emitWord = [&](String rawWord) {
        if (totalResponseLines >= kMaxResponseLines) return;

        while (rawWord.length() > SCREEN_WIDTH) {
            if (line.length() > 0) {
                addResponseLine(line);
                line = "";
                if (totalResponseLines >= kMaxResponseLines) return;
            }
            addResponseLine(rawWord.substring(0, SCREEN_WIDTH));
            rawWord = rawWord.substring(SCREEN_WIDTH);
            if (totalResponseLines >= kMaxResponseLines) return;
        }

        if (rawWord.length() == 0) return;

        if (line.length() == 0) {
            line = rawWord;
        } else if (line.length() + 1 + rawWord.length() <= SCREEN_WIDTH) {
            line += " " + rawWord;
        } else {
            addResponseLine(line);
            line = rawWord;
        }
    };

    for (unsigned int i = 0; i <= paragraph.length(); i++) {
        char c = (i < paragraph.length()) ? paragraph[i] : ' ';
        if (c == ' ') {
            if (word.length() > 0) {
                emitWord(word);
                word = "";
                if (totalResponseLines >= kMaxResponseLines) break;
            }
        } else {
            word += c;
        }
    }

    if (line.length() > 0 && totalResponseLines < kMaxResponseLines) addResponseLine(line);
}

void setResponse(const String &text) {
    clearResponseLines();

    String paragraph = "";
    for (unsigned int i = 0; i < text.length(); i++) {
        char c = text[i];
        if (c == '\r') continue;
        if (c == '\n') {
            wrapResponseParagraph(paragraph);
            paragraph = "";
            continue;
        }
        paragraph += c;
    }
    wrapResponseParagraph(paragraph);

    if (totalResponseLines == 0) addResponseLine("");
}

int maxTopLine() { return std::max(0, totalResponseLines - kResponseWindowLines); }

// ---------- test helpers ----------

struct Op {
    const char *name = "state";
    bool ok = true;
    String err = "";
    unsigned long capMs = 0;
    unsigned long apiMs = 0;
    size_t b64Len = 0;
    size_t jpegLen = 0;
    uint16_t jpegW = 0;
    uint16_t jpegH = 0;
    uint32_t heapBefore = 0;
    uint32_t psramBefore = 0;
};

bool isErrorText(const String &s) {
    return s.startsWith("Error:") || s.startsWith("API error") || s.startsWith("HTTP error") ||
           s == "DNS ERROR" || s == "DNS/NET ERROR";
}

int badBytes(const String &s) {
    int n = 0;
    for (unsigned int i = 0; i < s.length(); i++) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        if (c != 0x01 && (c < 32 || c > 126)) n++;   // 0x01 = the pi marker, fine
    }
    return n;
}

String jsonEscape(const String &s) {
    String out;
    out.reserve(s.length() + 16);
    for (unsigned int i = 0; i < s.length(); i++) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else out += static_cast<char>(c);
    }
    return out;
}

void addNum(String &j, const char *key, unsigned long v) {
    j += ",\""; j += key; j += "\":"; j += String(v);
}
void addBool(String &j, const char *key, bool v) {
    j += ",\""; j += key; j += "\":"; j += v ? "true" : "false";
}
void addStr(String &j, const char *key, const String &v) {
    j += ",\""; j += key; j += "\":\""; j += jsonEscape(v); j += '"';
}

String buildJson(const Op &op) {
    const int top = std::max(0, std::min(currentTopLine, maxTopLine()));
    String window = "";
    String rows = "[";
    for (int i = 0; i < kResponseWindowLines; i++) {
        const int idx = top + i;
        const String &row = (totalResponseLines > 0 && idx < totalResponseLines)
                                ? responseLines[idx] : blankResponseLine();
        window += row;
        if (i) rows += ',';
        rows += '"'; rows += jsonEscape(row); rows += '"';
    }
    rows += ']';

    String j;
    j.reserve(9000);
    j += "{\"op\":\""; j += op.name; j += '"';
    addBool(j, "ok", op.ok);
    addStr(j, "err", op.err);
    addNum(j, "capMs", op.capMs);
    addNum(j, "apiMs", op.apiMs);
    addNum(j, "b64Len", op.b64Len);
    addNum(j, "jpegLen", op.jpegLen);
    addNum(j, "jpegW", op.jpegW);
    addNum(j, "jpegH", op.jpegH);
    addNum(j, "heapBefore", op.heapBefore);
    addNum(j, "psramBefore", op.psramBefore);
    addNum(j, "heapNow", ESP.getFreeHeap());
    addNum(j, "psramNow", ESP.getFreePsram());
    addNum(j, "minHeap", ESP.getMinFreeHeap());
    addNum(j, "bigInternal", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    addBool(j, "wifi", WifiManager::isConnected());
    addStr(j, "ip", WiFi.localIP().toString());
    addNum(j, "rssi", static_cast<unsigned long>(-WiFi.RSSI()));   // shown as negative on the page
    addNum(j, "width", SCREEN_WIDTH);
    addNum(j, "top", top);
    addNum(j, "total", totalResponseLines);
    addNum(j, "maxLines", kMaxResponseLines);
    addBool(j, "truncated", totalResponseLines >= kMaxResponseLines);
    addNum(j, "badWindow", badBytes(window));
    addNum(j, "badText", badBytes(lastRaw));
    addNum(j, "rawLen", lastRaw.length());
    addStr(j, "raw", lastRaw);
    j += ",\"rows\":"; j += rows;
    j += '}';
    return j;
}

void respond(const Op &op) {
    Serial.printf("[%s] ok=%d cap=%lums api=%lums jpeg=%u b64=%u heap %u->%u min=%u err=%s\n",
                  op.name, (int)op.ok, op.capMs, op.apiMs, (unsigned)op.jpegLen, (unsigned)op.b64Len,
                  (unsigned)op.heapBefore, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
                  op.err.c_str());
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", buildJson(op));
}

Op startOp(const char *name) {
    Op op;
    op.name = name;
    op.heapBefore = ESP.getFreeHeap();
    op.psramBefore = ESP.getFreePsram();
    return op;
}

// ---------- actions (same order of steps as cmdSnapSolve / handleTextPrompt / scroll) ----------

void handleSnap() {
    Op op = startOp("snap");
    const int warm = constrain(server.arg("warm").toInt(), 0, 10);

    if (!WifiManager::isConnected()) {
        op.ok = false; op.err = "NO WIFI";
        respond(op);
        return;
    }

    const unsigned long t0 = millis();
    if (!Camera::init()) {
        op.ok = false; op.err = "CAMERA INIT FAIL";
        op.capMs = millis() - t0;
        respond(op);
        return;
    }
    for (int i = 0; i < warm; i++) {          // optional extra throw-away frames
        camera_fb_t *f = esp_camera_fb_get();
        if (f) esp_camera_fb_return(f);
    }

    String imageBase64 = Camera::captureBase64();   // the real call
    op.capMs = millis() - t0;
    op.b64Len = imageBase64.length();
    op.jpegLen = Camera::lastCaptureSize();
    op.jpegW = Camera::lastCaptureWidth();
    op.jpegH = Camera::lastCaptureHeight();
    if (imageBase64.length() == 0) {
        op.ok = false; op.err = "CAMERA FAIL";
        respond(op);
        return;
    }

    const unsigned long t1 = millis();
    const String raw = OpenAI::solveImage(imageBase64);   // the real call
    op.apiMs = millis() - t1;
    lastRaw = raw;
    setResponse(raw);
    if (isErrorText(raw)) { op.ok = false; op.err = raw.substring(0, 160); }
    respond(op);
}

void handleText(bool isReply) {
    Op op = startOp(isReply ? "reply" : "ask");
    const String text = server.arg("text");

    if (!WifiManager::isConnected()) { op.ok = false; op.err = "NO WIFI"; respond(op); return; }
    if (text.length() == 0) { op.ok = false; op.err = "EMPTY PROMPT"; respond(op); return; }

    const unsigned long t1 = millis();
    const String raw = isReply ? OpenAI::reply(text) : OpenAI::ask(text);   // the real calls
    op.apiMs = millis() - t1;
    lastRaw = raw;
    setResponse(raw);
    if (isErrorText(raw)) { op.ok = false; op.err = raw.substring(0, 160); }
    respond(op);
}

void handleScroll() {
    Op op = startOp("scroll");
    const bool down = server.arg("dir") != "up";

    if (totalResponseLines <= 0) {
        op.ok = false; op.err = "ERR:NO RESPONSE";
    } else if (down) {
        if (currentTopLine < maxTopLine()) currentTopLine++;
    } else {
        if (currentTopLine > 0) currentTopLine--;
    }
    respond(op);
}

void handleState() { respond(startOp("state")); }

void handleClear() {
    OpenAI::clearConversation();
    respond(startOp("clear"));
}

void sendJpeg(const uint8_t *buf, size_t len) {
    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(len);
    server.send(200, "image/jpeg", "");
    WiFiClient &client = server.client();
    size_t sent = 0;
    while (sent < len && client.connected()) {
        const size_t n = std::min<size_t>(1024, len - sent);
        const size_t w = client.write(buf + sent, n);
        if (w == 0) break;
        sent += w;
    }
}

void handleLastJpg() {
    if (!Camera::hasLastCapture()) { server.send(404, "text/plain", "no photo yet"); return; }
    sendJpeg(Camera::lastCaptureData(), Camera::lastCaptureSize());
}

void handlePreviewJpg() {
    uint8_t *buf = nullptr;
    size_t len = 0;
    if (!Camera::capturePreviewJpeg(&buf, &len)) { server.send(503, "text/plain", "capture failed"); return; }
    sendJpeg(buf, len);
    free(buf);
}

const char PAGE[] PROGMEM = R"rawpage(<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><title>TI-AI test</title>
<style>
:root{color-scheme:light dark;--bg:#fafafa;--fg:#1c1c1c;--card:#fff;--line:#d8d8d8;--mut:#666;--lcd:#b8c4a0;--lcdfg:#1a2210;--bad:#d11}
@media(prefers-color-scheme:dark){:root{--bg:#161616;--fg:#eee;--card:#222;--line:#3a3a3a;--mut:#999;--lcd:#8a9672;--lcdfg:#10160a}}
body{font:15px system-ui,sans-serif;background:var(--bg);color:var(--fg);margin:0;padding:12px 16px;max-width:680px}
h1{font-size:18px;margin:0 0 8px}
.card{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:10px;margin:10px 0}
button{font:inherit;padding:8px 12px;border:1px solid var(--line);border-radius:6px;background:var(--card);color:var(--fg);cursor:pointer}
button.p{background:#2563eb;color:#fff;border-color:#2563eb}
button:disabled{opacity:.45;cursor:wait}
input[type=text]{font:inherit;padding:7px;width:100%;box-sizing:border-box;border:1px solid var(--line);border-radius:6px;background:var(--card);color:var(--fg)}
.row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin:6px 0}
#scr{font:16px/1.25 ui-monospace,Menlo,Consolas,monospace;background:var(--lcd);color:var(--lcdfg);padding:10px;border-radius:4px;margin:0;white-space:pre;overflow-x:auto;min-height:6.5em}
.bad{color:#fff;background:var(--bad);border-radius:2px}
.mut{color:var(--mut);font-size:13px}
table{border-collapse:collapse;width:100%;font-size:13px}td{padding:2px 6px;border-bottom:1px solid var(--line)}td:first-child{color:var(--mut);width:48%}
.fail{color:var(--bad);font-weight:600}.good{color:#188038;font-weight:600}
#log div{font:12px ui-monospace,Menlo,monospace;padding:2px 0;border-bottom:1px solid var(--line)}
img{max-width:100%;border-radius:4px;display:block}
</style></head><body>
<h1>TI-84 AI test panel</h1>
<div class=mut id=top>connecting&hellip;</div>

<div class=card>
 <pre id=scr>(no response yet)</pre>
 <div class=mut id=pos></div>
 <div class=row><button id=up onclick="scroll_('up')">&#9650; Scroll up</button>
 <button id=dn onclick="scroll_('down')">&#9660; Scroll down</button></div>
</div>

<div class=card>
 <div class=row><button class=p id=snap onclick=snap()>Snap &amp; Solve</button>
 <label class=mut>throw-away frames first <select id=warm><option>0<option>2<option>4<option>6</select></label></div>
 <div class=row><input type=text id=txt placeholder="type a question (like Ask / Reply on the calculator)"></div>
 <div class=row><button id=ask onclick="ask('ask')">Ask (fresh)</button>
 <button id=rep onclick="ask('reply')">Reply</button>
 <button id=clr onclick=clearConv()>Clear conversation</button></div>
 <div class=mut id=busy></div>
</div>

<div class=card>
 <b>Repeat test</b> <span class=mut>(each snap uses your OpenAI credit)</span>
 <div class=row>Snaps: <input type=text id=n value=10 style="width:60px">
 <button id=go onclick=stress()>Run</button> <button id=stop onclick="stopFlag=true">Stop</button></div>
 <div id=stressOut class=mut></div>
</div>

<div class=card><b>Last operation</b><table id=stats></table></div>
<div class=card><b>Photo that was sent</b> <button onclick=preview()>Take preview now</button>
 <img id=pic alt=""><div class=mut id=picInfo></div></div>
<div class=card><b>Full text from OpenAI</b><pre id=raw class=mut style="white-space:pre-wrap;margin:6px 0 0"></pre></div>
<div class=card><b>Log</b><div id=log></div></div>

<script>
const $=id=>document.getElementById(id);
let busy=false,tick=null,t0=0,stopFlag=false;
const BTN=['up','dn','snap','ask','rep','clr','go'];
function esc(s){return s.replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));}
function mark(s){return esc(s).replace(/[^\x20-\x7E\x01]/g,c=>'<b class=bad title="U+'+c.codePointAt(0).toString(16)+'">'+(c.trim()?c:'?')+'</b>').replace(/\x01/g,'\u03c0');}
function setBusy(b,label){
  busy=b;BTN.forEach(i=>$(i).disabled=b);
  clearInterval(tick);
  if(b){t0=Date.now();$('busy').textContent=label+' ...';
    tick=setInterval(()=>$('busy').textContent=label+' ... '+Math.round((Date.now()-t0)/1000)+' s',500);}
  else $('busy').textContent='';
}
async function api(path,opt,label){
  setBusy(true,label||path);const s=performance.now();
  try{const r=await fetch(path,opt);const j=await r.json();j.rtt=Math.round(performance.now()-s);return j;}
  catch(e){return {op:path,ok:false,netfail:true,err:'NO REPLY FROM ESP32 ('+e+')',rtt:Math.round(performance.now()-s)};}
  finally{setBusy(false);}
}
function log(j){
  const d=document.createElement('div');
  d.innerHTML=new Date().toLocaleTimeString()+' '+esc(j.op)+' '+(j.ok?'<span class=good>ok</span>':'<span class=fail>FAIL</span>')+
   ' '+(j.rtt/1000).toFixed(1)+'s '+esc(j.err||'');
  $('log').prepend(d);
}
function show(j){
  if(j.netfail||!j.rows)return;
  $('scr').innerHTML=j.total?j.rows.map(mark).join('\n'):'(no response yet)';
  $('pos').textContent=j.total?('lines '+(j.top+1)+'-'+Math.min(j.top+5,j.total)+' of '+j.total+
    (j.truncated?'  - CUT OFF at '+j.maxLines+' lines, the rest of the answer is dropped':'')):'';
  $('top').textContent=(j.wifi?'WiFi ok ':'WiFi DOWN ')+j.ip+'  '+(-j.rssi)+' dBm   heap '+j.heapNow+'  psram '+j.psramNow;
  const R=[['operation',j.op+' - '+(j.ok?'ok':'FAIL')],['error',j.err||'-'],
   ['round trip',(j.rtt/1000).toFixed(1)+' s'],['camera time',j.capMs+' ms'],['OpenAI time',j.apiMs+' ms'],
   ['JPEG',j.jpegLen?(j.jpegLen+' bytes, '+j.jpegW+'x'+j.jpegH):'-'],['base64 size',j.b64Len||'-'],
   ['heap before / now',j.heapBefore+' / '+j.heapNow],['lowest heap ever',j.minHeap],
   ['largest internal block',j.bigInternal],['psram before / now',j.psramBefore+' / '+j.psramNow],
   ['leftover odd chars in window',j.badWindow+(j.badWindow?'  (shown in red above)':'')],
   ['non-ASCII bytes in OpenAI answer (converted)',j.badText],['answer length',j.rawLen+' chars']];
  $('stats').innerHTML=R.map(r=>'<tr><td>'+r[0]+'<td>'+esc(String(r[1]))).join('');
  $('raw').innerHTML=mark(j.raw||'');
}
async function run(path,opt,label){const j=await api(path,opt,label);log(j);show(j);return j;}
async function snap(){
  const j=await run('/api/snap?warm='+$('warm').value,undefined,'Snap & Solve');
  if(j.jpegLen){$('pic').src='/api/last.jpg?'+Date.now();$('picInfo').textContent=j.jpegLen+' bytes, '+j.jpegW+'x'+j.jpegH;}
  return j;
}
function ask(kind){const t=$('txt').value;
  return run('/api/'+kind,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'text='+encodeURIComponent(t)},kind);}
function scroll_(d){return run('/api/scroll?dir='+d,undefined,'scroll');}
function clearConv(){return run('/api/clear',undefined,'clear');}
function preview(){$('pic').src='/api/preview.jpg?'+Date.now();$('picInfo').textContent='preview (not sent to OpenAI)';}
async function stress(){
  const n=Math.max(1,Math.min(50,+$('n').value||10));stopFlag=false;let ok=0,fail=0,net=0;const errs={};const times=[];
  for(let i=1;i<=n&&!stopFlag;i++){
    $('stressOut').textContent='running '+i+' of '+n+'  ok '+ok+'  failed '+fail;
    const j=await snap();
    if(j.ok){ok++;times.push(j.rtt);}else{fail++;if(j.netfail)net++;const k=(j.err||'?').slice(0,60);errs[k]=(errs[k]||0)+1;}
    await new Promise(r=>setTimeout(r,1500));
  }
  const avg=times.length?(times.reduce((a,b)=>a+b)/times.length/1000).toFixed(1)+' s avg':'';
  $('stressOut').innerHTML='done: '+ok+' ok, '+fail+' failed ('+net+' with no reply from the ESP32) '+avg+'<br>'+
    Object.entries(errs).map(e=>e[1]+'x '+esc(e[0])).join('<br>');
}
api('/api/state').then(show);
</script></body></html>)rawpage";

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("\n=== TI-84 AI test panel (apptest) ===");
    Serial.printf("PSRAM found=%d size=%u heap free=%u\n",
                  (int)psramFound(), (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreeHeap());

    clearResponseLines();
    WifiManager::loadCredentials();
    Camera::applyQualityProfile(WifiManager::cameraQualityProfile());

    // To join a DIFFERENT WiFi: press the BOOT button during these 3 seconds after reset.
    bool forcePortal = false;
    if (WifiManager::hasCredentials()) {
        pinMode(0, INPUT_PULLUP);
        Serial.println("Press the BOOT button within 3 seconds to change WiFi...");
        const unsigned long w = millis();
        while (millis() - w < 3000) {
            if (digitalRead(0) == LOW) { forcePortal = true; break; }
            delay(10);
        }
    }

    if (!WifiManager::hasCredentials() || forcePortal) {
        // Same setup portal the real firmware uses. Join the WiFi it creates, enter the new
        // WiFi name + password (leave the API key blank to keep the saved one), press save.
        // The board restarts by itself and uses the new WiFi.
        Serial.println("Starting the setup portal (same as the calculator's CONFIGURE).");
        WifiManager::startConfigPortal();
        unsigned long lastMsg = 0;
        while (true) {
            WifiManager::handleConfigPortal();
            if (millis() - lastMsg > 5000) {
                lastMsg = millis();
                Serial.println("Waiting for you to fill in the setup portal...");
            }
            delay(10);
        }
    }

    if (!WifiManager::beginConnect()) {
        Serial.println("beginConnect failed (no WiFi config?)");
    } else {
        const unsigned long start = millis();
        while (WifiManager::isConnectPending() && millis() - start < 30000) {
            WifiManager::pollConnect();
            delay(100);
        }
    }

    server.on("/", []() { server.send_P(200, "text/html", PAGE); });
    server.on("/api/state", handleState);
    server.on("/api/snap", handleSnap);
    server.on("/api/ask", HTTP_POST, []() { handleText(false); });
    server.on("/api/reply", HTTP_POST, []() { handleText(true); });
    server.on("/api/scroll", handleScroll);
    server.on("/api/clear", handleClear);
    server.on("/api/last.jpg", handleLastJpg);
    server.on("/api/preview.jpg", handlePreviewJpg);
    server.begin();

    Serial.printf("WiFi connected=%d\n", (int)WifiManager::isConnected());
    Serial.printf("Open  http://%s/  from a phone or computer on the same WiFi\n",
                  WiFi.localIP().toString().c_str());
}

void loop() {
    server.handleClient();

    if (!WifiManager::isConnected()) {
        if (WifiManager::isConnectPending()) {
            WifiManager::pollConnect();
        } else if (millis() - lastReconnectTry > 10000) {
            lastReconnectTry = millis();
            Serial.println("WiFi down, reconnecting...");
            WifiManager::beginConnect();
        }
    }
}
