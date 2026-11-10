// camtool.cpp - standalone camera viewer with live settings, for XIAO ESP32S3 Sense.
// Starts its own WiFi network (CAMTOOL, password 12345678). Join it from a
// phone or laptop and open http://192.168.4.1
// Not part of the calculator firmware: use it by itself, then switch back.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "esp_camera.h"

static camera_config_t cfg = {};
static WebServer server(80);
static framesize_t maxSize = FRAMESIZE_QVGA;

static const char PAGE[] PROGMEM = R"rawliteral(
<!doctype html><html><head>
<meta name=viewport content="width=device-width,initial-scale=1">
<style>
body{font-family:sans-serif;margin:8px;max-width:640px}
img{width:100%;background:#222;min-height:120px}
label{display:block;margin:8px 0 0}
input[type=range]{width:100%}
textarea{width:100%;height:170px;font-family:monospace}
span{color:#b00}
</style></head><body>
<h3>Camera tool</h3>
<img id=pic>
<div id=ctl></div>
<h4>Lines for camera.h (put them with the other s-&gt; lines)</h4>
<textarea id=out readonly></textarea>
<script>
const S=[['brightness',-2,2,1],['contrast',-2,2,1],['saturation',-2,2,0],
 ['sharpness',-3,3,0],['ae_level',-2,2,0],['quality',6,40,12],
 ['aec_value',0,1200,300],['agc_gain',0,30,0]];
const C=[['vflip',1],['hmirror',0],['whitebal',1],['awb_gain',1],
 ['exposure_ctrl',1],['gain_ctrl',1]];
const v={};
function send(n,x){
  v[n]=x;
  fetch('/set?var='+n+'&val='+x).then(r=>r.text()).then(t=>{
    const e=document.getElementById('st_'+n);
    if(e) e.textContent=(t==='-1')?' (not supported)':'';
  });
  lines();
}
function lines(){
  let t='// frame size and quality come from the calculator preset\n';
  for(const n of ['brightness','contrast','saturation','sharpness','ae_level'])
    t+='s->set_'+n+'(s, '+v[n]+');\n';
  for(const c of C) t+='s->set_'+c[0]+'(s, '+v[c[0]]+');\n';
  if(+v.exposure_ctrl===0) t+='s->set_aec_value(s, '+v.aec_value+');\n';
  if(+v.gain_ctrl===0) t+='s->set_agc_gain(s, '+v.agc_gain+');\n';
  document.getElementById('out').value=t;
}
let h='<label>Size <select onchange="send(\'framesize\',this.value)">'+
 '<option>QQVGA<option>QVGA<option>CIF<option selected>VGA<option>SVGA</select>'+
 '<span id=st_framesize></span></label>';
for(const s of S){
  v[s[0]]=s[3];
  h+='<label>'+s[0]+': <b id=val_'+s[0]+'>'+s[3]+'</b><span id=st_'+s[0]+'></span>'+
   '<input type=range min='+s[1]+' max='+s[2]+' value='+s[3]+
   ' oninput="document.getElementById(\'val_'+s[0]+'\').textContent=this.value"'+
   ' onchange="send(\''+s[0]+'\',this.value)"></label>';
}
for(const c of C){
  v[c[0]]=c[1];
  h+='<label><input type=checkbox '+(c[1]?'checked':'')+
   ' onchange="send(\''+c[0]+'\',this.checked?1:0)"> '+c[0]+
   '<span id=st_'+c[0]+'></span></label>';
}
document.getElementById('ctl').innerHTML=h;
lines();
const pic=document.getElementById('pic');
function next(){pic.src='/capture?'+Date.now();}
pic.onload=()=>setTimeout(next,150);
pic.onerror=()=>setTimeout(next,1000);
next();
</script></body></html>
)rawliteral";

static bool sizeFromName(const String &n, framesize_t &out) {
    if (n == "QQVGA") out = FRAMESIZE_QQVGA;
    else if (n == "QVGA") out = FRAMESIZE_QVGA;
    else if (n == "CIF") out = FRAMESIZE_CIF;
    else if (n == "VGA") out = FRAMESIZE_VGA;
    else if (n == "SVGA") out = FRAMESIZE_SVGA;
    else return false;
    return true;
}

static void handleRoot() {
    server.send_P(200, "text/html", PAGE);
}

static void handleCapture() {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        server.send(503, "text/plain", "capture failed");
        return;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(fb->len);
    server.send(200, "image/jpeg", "");
    server.client().write(fb->buf, fb->len);
    esp_camera_fb_return(fb);
}

static void handleSet() {
    String var = server.arg("var");
    String sval = server.arg("val");
    int val = sval.toInt();
    sensor_t *s = esp_camera_sensor_get();
    int res = -1;
    if (s) {
        if (var == "framesize") {
            framesize_t fs;
            if (sizeFromName(sval, fs) && (int)fs <= (int)maxSize) {
                res = s->set_framesize(s, fs);
            }
        }
        else if (var == "quality")       res = s->set_quality(s, val);
        else if (var == "brightness")    res = s->set_brightness(s, val);
        else if (var == "contrast")      res = s->set_contrast(s, val);
        else if (var == "saturation")    res = s->set_saturation(s, val);
        else if (var == "sharpness")     res = s->set_sharpness(s, val);
        else if (var == "ae_level")      res = s->set_ae_level(s, val);
        else if (var == "aec_value")     res = s->set_aec_value(s, val);
        else if (var == "agc_gain")      res = s->set_agc_gain(s, val);
        else if (var == "vflip")         res = s->set_vflip(s, val);
        else if (var == "hmirror")       res = s->set_hmirror(s, val);
        else if (var == "whitebal")      res = s->set_whitebal(s, val);
        else if (var == "awb_gain")      res = s->set_awb_gain(s, val);
        else if (var == "exposure_ctrl") res = s->set_exposure_ctrl(s, val);
        else if (var == "gain_ctrl")     res = s->set_gain_ctrl(s, val);
    }
    Serial.printf("set %s=%s -> %d\n", var.c_str(), sval.c_str(), res);
    server.send(200, "text/plain", String(res));
}

void setup() {
    Serial.begin(115200);
    delay(1500);

    // XIAO ESP32S3 Sense camera pins
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer   = LEDC_TIMER_0;
    cfg.pin_d0 = 15; cfg.pin_d1 = 17; cfg.pin_d2 = 18; cfg.pin_d3 = 16;
    cfg.pin_d4 = 14; cfg.pin_d5 = 12; cfg.pin_d6 = 11; cfg.pin_d7 = 48;
    cfg.pin_xclk = 10; cfg.pin_pclk = 13; cfg.pin_vsync = 38; cfg.pin_href = 47;
    cfg.pin_sccb_sda = 40; cfg.pin_sccb_scl = 39;
    cfg.pin_pwdn = -1; cfg.pin_reset = -1;
    cfg.xclk_freq_hz = 20000000;
    cfg.pixel_format = PIXFORMAT_JPEG;
    cfg.jpeg_quality = 12;
    cfg.fb_count     = 2;
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode    = CAMERA_GRAB_LATEST;

    // Start at the largest size that works, so the size menu can go up to it
    static const framesize_t tries[] = {FRAMESIZE_SVGA, FRAMESIZE_VGA, FRAMESIZE_QVGA};
    esp_err_t err = ESP_FAIL;
    for (size_t i = 0; i < sizeof(tries) / sizeof(tries[0]); i++) {
        cfg.frame_size = tries[i];
        err = esp_camera_init(&cfg);
        if (err == ESP_OK) {
            maxSize = tries[i];
            break;
        }
        Serial.printf("init failed 0x%x at frame size %d\n", err, (int)tries[i]);
        esp_camera_deinit();
        delay(200);
    }
    if (err != ESP_OK) {
        while (true) {
            Serial.println("Camera init failed at every size. Check the ribbon and reset.");
            delay(3000);
        }
    }

    sensor_t *s = esp_camera_sensor_get();
    Serial.printf("Camera OK, sensor PID=0x%X, max size %d\n", s->id.PID, (int)maxSize);
    if ((int)maxSize >= (int)FRAMESIZE_VGA) s->set_framesize(s, FRAMESIZE_VGA);

    // Same starting values as the firmware and the web page
    s->set_brightness(s, 1);
    s->set_contrast(s, 1);
    s->set_saturation(s, 0);
    s->set_sharpness(s, 0);
    s->set_ae_level(s, 0);
    s->set_vflip(s, 1);
    s->set_hmirror(s, 0);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_gain_ctrl(s, 1);

    WiFi.mode(WIFI_AP);
    WiFi.softAP("CAMTOOL", "12345678");
    Serial.printf("Join WiFi CAMTOOL (password 12345678), then open http://%s\n",
                  WiFi.softAPIP().toString().c_str());

    server.on("/", handleRoot);
    server.on("/capture", handleCapture);
    server.on("/set", handleSet);
    server.begin();
}

void loop() {
    server.handleClient();
}
