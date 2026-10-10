// Pole node: open Wi-Fi + captive SOS page, forwards SOS to central over ESP-NOW.
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_now.h>
#include <esp_wifi.h>

#define NODE_ID 1
#define CHANNEL 1  // AP and ESP-NOW must share this channel

enum Kind : uint8_t { SOS = 1, ACK = 2, BEAT = 3 };

struct __attribute__((packed)) Msg {
  uint8_t kind;
  uint16_t node;
  uint32_t id;
  uint8_t type;    // 1 injured, 2 flood, 3 fire, 4 trapped, 5 food/water, 6 child/elderly, 7 other
  uint8_t people;  // 1, 2 (2-5), 3 (6-10), 4 (10+)
  char note[120];
};

const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

DNSServer dns;
WebServer web(80);

// Small table of recent SOS so the phone can poll delivery status.
struct Pending { uint32_t id; Msg msg; bool acked; uint8_t tries; uint32_t lastTry; };
Pending pending[8];
uint8_t pendingNext = 0;

const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang=en><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1"><meta name=theme-color content="#d7261e"><title>SOS - Emergency help</title>
<style>
:root{--ink:#16212b;--mute:#5b6874;--line:#d9dee3;--bg:#fff;--red:#d7261e;--ok:#11875d}
*{box-sizing:border-box}
body{margin:0;font-family:system-ui,-apple-system,"Segoe UI",Roboto,"Noto Sans","Noto Sans Devanagari",sans-serif;background:var(--bg);color:var(--ink);-webkit-tap-highlight-color:transparent}
.top{background:var(--red);color:#fff;padding:18px 16px 16px}
.top h1{margin:0;font-size:30px;font-weight:900;letter-spacing:-.5px;line-height:1.05}
.top p{margin:6px 0 0;font-size:15px;opacity:.92}
.wrap{padding:4px 16px 120px;max-width:520px;margin:0 auto}
h2{display:flex;align-items:center;gap:10px;font-size:19px;font-weight:800;margin:22px 0 10px}
h2 b{display:inline-grid;place-items:center;width:28px;height:28px;border-radius:50%;background:var(--ink);color:#fff;font-size:15px}
h2 small{font-weight:500;color:var(--mute);font-size:15px}
.g{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.g.p{grid-template-columns:repeat(4,1fr)}
#t .b:last-child{grid-column:span 2;min-height:72px}
.b{position:relative;display:flex;flex-direction:column;align-items:flex-start;gap:2px;min-height:96px;padding:12px;border:2px solid var(--line);border-radius:14px;background:#fff;color:var(--ink);font:inherit;text-align:left;cursor:pointer}
.b i{font-style:normal;font-size:34px;line-height:1;margin-bottom:6px}
.b span{font-size:17px;font-weight:800;line-height:1.15}
.b em{font-style:normal;font-size:14px;color:var(--mute)}
.b::before{content:"";position:absolute;left:0;top:12px;bottom:12px;width:5px;border-radius:0 4px 4px 0;background:var(--c)}
.p .b{min-height:72px;align-items:center;justify-content:center;padding:8px 4px}
.p .b::before{display:none}.p .b span{font-size:22px}
.p .b[aria-pressed=true]::after{top:3px;right:6px;font-size:13px}
.b[aria-pressed=true]{border-color:var(--ink);background:#f2f4f6;box-shadow:inset 0 0 0 2px var(--ink)}
.b[aria-pressed=true]::after{content:"\2713";position:absolute;top:8px;right:10px;font-weight:900;font-size:18px}
.b:focus-visible,#send:focus-visible,textarea:focus-visible{outline:3px solid #1d6fd8;outline-offset:2px}
textarea{width:100%;font:inherit;font-size:17px;padding:12px;border:2px solid var(--line);border-radius:14px;resize:none}
.bar{position:fixed;left:0;right:0;bottom:0;padding:12px 16px calc(12px + env(safe-area-inset-bottom));background:#fff;border-top:1px solid var(--line)}
#send{display:block;width:100%;max-width:520px;margin:0 auto;padding:18px;border:0;border-radius:14px;background:var(--red);color:#fff;font:inherit;font-size:22px;font-weight:900}
#send:disabled{background:#c9ced3;color:#6b7680}
#done{display:none;position:fixed;inset:0;background:var(--ink);color:#fff;padding:40px 24px;text-align:center;flex-direction:column;justify-content:center;align-items:center}
#done.show{display:flex}
#done .ic{font-size:72px;line-height:1}
#done h3{font-size:28px;font-weight:900;margin:18px 0 8px;max-width:340px}
#done p{font-size:17px;line-height:1.5;margin:0;max-width:340px;opacity:.85}
#done.ok{background:var(--ok)}
#done button{margin-top:28px;padding:14px 22px;border:2px solid #fff;border-radius:12px;background:none;color:#fff;font:inherit;font-weight:700}
.dots::after{content:"";animation:d 1.2s steps(4) infinite}
@keyframes d{0%{content:""}25%{content:"."}50%{content:".."}75%{content:"..."}}
@media (prefers-reduced-motion:reduce){.dots::after{animation:none;content:"..."}}
</style></head><body>
<div class=top><h1>Emergency help<br>आपातकालीन मदद</h1><p>PahadNet works without internet. Your message goes to the control room from pole %NODE%.</p></div>
<div class=wrap>
<h2><b>1</b>What happened? <small>क्या हुआ?</small></h2>
<div class=g id=t>
<button class=b data-v=1 aria-pressed=false style="--c:#d7261e"><i>&#x1FA78;</i><span>Injured</span><em>घायल</em></button>
<button class=b data-v=2 aria-pressed=false style="--c:#1d6fd8"><i>&#x1F30A;</i><span>Flood</span><em>बाढ़</em></button>
<button class=b data-v=3 aria-pressed=false style="--c:#e8590c"><i>&#x1F525;</i><span>Fire</span><em>आग</em></button>
<button class=b data-v=4 aria-pressed=false style="--c:#7a5230"><i>&#x1F3DA;</i><span>Trapped</span><em>फँसे हुए</em></button>
<button class=b data-v=5 aria-pressed=false style="--c:#2f9e44"><i>&#x1F35E;</i><span>Food, water</span><em>खाना, पानी</em></button>
<button class=b data-v=6 aria-pressed=false style="--c:#9c36b5"><i>&#x1F476;</i><span>Child, elderly</span><em>बच्चे, बुज़ुर्ग</em></button>
<button class=b data-v=7 aria-pressed=false style="--c:#5b6874"><i>&#x2753;</i><span>Something else</span><em>कुछ और</em></button>
</div>
<h2><b>2</b>How many people? <small>कितने लोग?</small></h2>
<div class="g p" id=p>
<button class=b data-v=1 aria-pressed=false><span>1</span></button>
<button class=b data-v=2 aria-pressed=false><span>2-5</span></button>
<button class=b data-v=3 aria-pressed=false><span>6-10</span></button>
<button class=b data-v=4 aria-pressed=false><span>10+</span></button>
</div>
<h2><b>3</b>Anything else? <small>optional</small></h2>
<textarea id=n maxlength=110 rows=3 placeholder="Where exactly, who needs help. Any language."></textarea>
</div>
<div class=bar><button id=send disabled>Pick what happened</button></div>
<div id=done role=status aria-live=polite><div class=ic id=ic>&#x1F4E1;</div><h3 id=dh class=dots>Sending your SOS</h3><p id=dp>Passing your message pole to pole. Keep this page open.</p><button id=again hidden>Send another SOS</button></div>
<script>
var sel={t:0,p:0},$=function(i){return document.getElementById(i)},send=$('send'),done=$('done');
function upd(){send.disabled=!(sel.t&&sel.p);send.textContent=!sel.t?'Pick what happened':!sel.p?'Pick how many people':'Send SOS';}
['t','p'].forEach(function(g){[].forEach.call(document.querySelectorAll('#'+g+' .b'),function(b){b.onclick=function(){
[].forEach.call(document.querySelectorAll('#'+g+' .b'),function(x){x.setAttribute('aria-pressed','false')});
b.setAttribute('aria-pressed','true');sel[g]=b.dataset.v;upd();};});});
function show(cls,ic,h,p,dots,again){done.className='show '+cls;$('ic').innerHTML=ic;$('dh').textContent=h;$('dh').className=dots?'dots':'';$('dp').textContent=p;$('again').hidden=!again;}
send.onclick=function(){send.disabled=true;show('','&#x1F4E1;','Sending your SOS','Passing your message pole to pole. Keep this page open.',1,0);
fetch('/sos?t='+sel.t+'&p='+sel.p+'&n='+encodeURIComponent($('n').value)).then(function(r){return r.text()}).then(poll)
.catch(function(){show('','&#x26A0;','Not sent','The pole did not answer. Move closer to it and try again.',0,1);});};
function poll(id){fetch('/status?id='+id).then(function(r){return r.text()}).then(function(s){
if(s=='acked')show('ok','&#x2705;','Control room has your SOS','Stay where you are if it is safe. If you move, send a new SOS from the next pole.',0,1);
else if(s=='failed')show('','&#x26A0;','Not delivered yet','The control room did not confirm. Try again in a moment.',0,1);
else setTimeout(function(){poll(id)},1000);}).catch(function(){setTimeout(function(){poll(id)},1500)});}
$('again').onclick=function(){done.className='';upd();};
</script></body></html>)HTML";

void sendMsg(const Msg &m) {
  esp_now_send(BCAST, (const uint8_t *)&m, sizeof(m));
}

void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(Msg)) return;
  const Msg *m = (const Msg *)data;
  if (m->kind != ACK || m->node != NODE_ID) return;
  for (auto &p : pending)
    if (p.id == m->id) p.acked = true;
}

// Strip characters that would break the JSON line the central prints.
void cleanNote(const String &in, char *out, size_t cap) {
  size_t j = 0;
  for (size_t i = 0; i < in.length() && j < cap - 1; i++) {
    char c = in[i];
    if (c == '"' || c == '\\' || (uint8_t)c < 0x20) c = ' ';
    out[j++] = c;
  }
  out[j] = 0;
}

void handleSos() {
  int t = web.arg("t").toInt(), p = web.arg("p").toInt();
  if (t < 1 || t > 7 || p < 1 || p > 4) { web.send(400, "text/plain", "bad"); return; }
  Pending &slot = pending[pendingNext++ % 8];
  memset(&slot, 0, sizeof(slot));
  slot.id = esp_random();
  slot.msg.kind = SOS;
  slot.msg.node = NODE_ID;
  slot.msg.id = slot.id;
  slot.msg.type = t;
  slot.msg.people = p;
  cleanNote(web.arg("n"), slot.msg.note, sizeof(slot.msg.note));
  web.send(200, "text/plain", String(slot.id));
}

void handleStatus() {
  uint32_t id = strtoul(web.arg("id").c_str(), nullptr, 10);
  for (auto &p : pending)
    if (p.id == id) {
      web.send(200, "text/plain", p.acked ? "acked" : (p.tries >= 10 ? "failed" : "sending"));
      return;
    }
  web.send(200, "text/plain", "failed");
}

void handlePage() {
  String page = FPSTR(PAGE);
  page.replace("%NODE%", String(NODE_ID));
  web.send(200, "text/html", page);
}

// Any unknown URL (incl. OS connectivity checks) goes to the SOS page -> captive popup.
void redirect() {
  web.sendHeader("Location", "http://192.168.4.1/", true);
  web.send(302, "text/plain", "");
}

void setup() {
  Serial.begin(115200);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(("SOS-PahadNet-" + String(NODE_ID)).c_str(), nullptr, CHANNEL);  // open network
  esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);

  dns.start(53, "*", WiFi.softAPIP());

  web.on("/", handlePage);
  web.on("/sos", handleSos);
  web.on("/status", handleStatus);
  web.onNotFound(redirect);
  web.begin();

  esp_now_init();
  esp_now_register_recv_cb(onRecv);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST, 6);
  peer.channel = CHANNEL;
  peer.ifidx = WIFI_IF_AP;
  esp_now_add_peer(&peer);

  Serial.printf("pole %d up, AP %s\n", NODE_ID, WiFi.softAPIP().toString().c_str());
}

uint32_t lastBeat = 0;

void loop() {
  dns.processNextRequest();
  web.handleClient();

  uint32_t now = millis();
  // Retry unacked SOS every second, up to 10 times.
  for (auto &p : pending) {
    if (p.id && !p.acked && p.tries < 10 && now - p.lastTry > 1000) {
      sendMsg(p.msg);
      p.tries++;
      p.lastTry = now;
    }
  }
  if (now - lastBeat > 5000) {
    Msg b = {};
    b.kind = BEAT;
    b.node = NODE_ID;
    b.people = WiFi.softAPgetStationNum();  // reuse field: phones connected
    sendMsg(b);
    lastBeat = now;
  }
}
