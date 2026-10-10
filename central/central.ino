// Central node: receives ESP-NOW from poles, ACKs SOS, prints one JSON line per event to serial.
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#define CHANNEL 1

enum Kind : uint8_t { SOS = 1, ACK = 2, BEAT = 3 };

struct __attribute__((packed)) Msg {
  uint8_t kind;
  uint16_t node;
  uint32_t id;
  uint8_t type;
  uint8_t people;
  char note[120];
};

struct Rx { Msg msg; int8_t rssi; uint8_t mac[6]; };

const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
QueueHandle_t rxq;

// Runs in the Wi-Fi task: just copy and hand off to loop().
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(Msg)) return;
  Rx rx;
  memcpy(&rx.msg, data, sizeof(Msg));
  rx.msg.note[sizeof(rx.msg.note) - 1] = 0;
  rx.rssi = info->rx_ctrl->rssi;
  memcpy(rx.mac, info->src_addr, 6);
  xQueueSend(rxq, &rx, 0);
}

// Pole retries until ACKed, so drop repeats of the same id.
uint32_t seen[32];
uint8_t seenNext = 0;
bool alreadySeen(uint32_t id) {
  for (auto s : seen)
    if (s == id) return true;
  seen[seenNext++ % 32] = id;
  return false;
}

void setup() {
  Serial.begin(115200);
  rxq = xQueueCreate(16, sizeof(Rx));
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);

  esp_now_init();
  esp_now_register_recv_cb(onRecv);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST, 6);
  peer.channel = CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  esp_now_add_peer(&peer);

}

uint32_t lastHello = 0;

void loop() {
  // Lets hub.py find this board among all serial ports.
  if (millis() - lastHello > 3000) {
    Serial.println("{\"kind\":\"central\"}");
    lastHello = millis();
  }
  Rx rx;
  if (xQueueReceive(rxq, &rx, pdMS_TO_TICKS(50)) != pdTRUE) return;
  Msg &m = rx.msg;
  char mac[18];
  snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
           rx.mac[0], rx.mac[1], rx.mac[2], rx.mac[3], rx.mac[4], rx.mac[5]);

  if (m.kind == BEAT) {
    Serial.printf("{\"kind\":\"beat\",\"node\":%u,\"phones\":%u,\"rssi\":%d,\"mac\":\"%s\"}\n",
                  m.node, m.people, rx.rssi, mac);
  } else if (m.kind == SOS) {
    Msg ack = {};
    ack.kind = ACK;
    ack.node = m.node;
    ack.id = m.id;
    esp_now_send(BCAST, (const uint8_t *)&ack, sizeof(ack));  // ACK every copy in case one was lost
    if (alreadySeen(m.id)) return;
    Serial.printf("{\"kind\":\"sos\",\"node\":%u,\"id\":%lu,\"type\":%u,\"people\":%u,\"note\":\"%s\",\"rssi\":%d,\"mac\":\"%s\"}\n",
                  m.node, (unsigned long)m.id, m.type, m.people, m.note, rx.rssi, mac);
  }
}
