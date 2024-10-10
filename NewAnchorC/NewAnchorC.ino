#include <WiFi.h>
#include <time.h>
#include <PubSubClient.h>

#include <DW1000Ng.hpp>
#include <DW1000NgUtils.hpp>
#include <DW1000NgRanging.hpp>

#include "moduleConfig.h"


// NTP 서버 설정 (기본적으로 pool.ntp.org 사용)
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0;      // GMT +0 (UTC 기준)
const int daylightOffset_sec = 0;  // Daylight Saving 시간 없음

// MQTT 토픽 설정
const char* mqtt_topic = "myRange/C";

WiFiClient espClient;
PubSubClient client(espClient);

const int SLOT_DURATION_MS = 200;  // 타임 슬롯 길이 (200ms)
const int NUM_ANCHORS = 3;         // 총 3개의 Anchor 사용
const int ANCHOR_A_SLOT = 0;       // Anchor A 동작 (0~200ms)
const int ANCHOR_B_SLOT = 1;       // Anchor B 동작 (200~400ms)
const int ANCHOR_C_SLOT = 2;       // Anchor C 동작 (400~600ms)

// connection pins
const uint8_t PIN_SCK = 18;   // SPI 클럭 핀
const uint8_t PIN_MOSI = 23;  // SPI 데이터 출력 핀
const uint8_t PIN_MISO = 19;  // SPI 데이터 입력 핀
const uint8_t PIN_SS = 4;     // SPI 슬레이브 선택 핀
const uint8_t PIN_RST = 15;   // 모듈 리셋 핀
const uint8_t PIN_IRQ = 17;   // 인터럽트 핀

// messages used in the ranging protocol
// TODO replace by enum
#define POLL 0
#define POLL_ACK 1
#define RANGE 2
#define RANGE_REPORT 3
#define RANGE_FAILED 255
// message flow state
volatile byte expectedMsgId = POLL;
// message sent/received state
volatile boolean sentAck = false;
volatile boolean receivedAck = false;
// protocol error state
boolean protocolFailed = false;
// timestamps to remember
uint64_t timePollSent;
uint64_t timePollReceived;
uint64_t timePollAckSent;
uint64_t timePollAckReceived;
uint64_t timeRangeSent;
uint64_t timeRangeReceived;

uint64_t timeComputedRange;
// last computed range/time
// data buffer
#define LEN_DATA 16
byte data[LEN_DATA];
// watchdog and reset period
uint32_t lastActivity;
uint32_t resetPeriod = 250;
// reply times (same on both sides for symm. ranging)
uint16_t replyDelayTimeUS = 3000;
// ranging counter (per second)
uint16_t successRangingCount = 0;
uint32_t rangingCountPeriod = 0;
float samplingRate = 0;

device_configuration_t DEFAULT_CONFIG = {
  false,
  true,
  true,
  true,
  false,
  SFDMode::STANDARD_SFD,
  Channel::CHANNEL_5,
  DataRate::RATE_850KBPS,
  PulseFrequency::FREQ_16MHZ,
  PreambleLength::LEN_256,
  PreambleCode::CODE_3
};

interrupt_configuration_t DEFAULT_INTERRUPT_CONFIG = {
  true,
  true,
  true,
  false,
  true
};

void setup() {
  // DEBUG monitoring
  Serial.begin(115200);
  delay(1000);

  // WiFi 연결
  setup_wifi();

  // NTP 서버와 시간 동기화
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // MQTT 브로커 연결
  client.setServer(MQTT_SERVER, MQTT_PORT);

  Serial.println(F("### DW1000Ng-arduino-ranging-anchorC ###"));
  // initialize the driver
  DW1000Ng::initialize(PIN_SS, PIN_IRQ, PIN_RST);
  Serial.println(F("DW1000Ng initialized ..."));
  // general configuration
  DW1000Ng::applyConfiguration(DEFAULT_CONFIG);
  DW1000Ng::applyInterruptConfiguration(DEFAULT_INTERRUPT_CONFIG);

  DW1000Ng::setDeviceAddress(3);

  DW1000Ng::setAntennaDelay(16436);

  Serial.println(F("Committed configuration ..."));
  // DEBUG chip info and registers pretty printed
  char msg[128];
  DW1000Ng::getPrintableDeviceIdentifier(msg);
  Serial.print("Device ID: ");
  Serial.println(msg);
  DW1000Ng::getPrintableExtendedUniqueIdentifier(msg);
  Serial.print("Unique ID: ");
  Serial.println(msg);
  DW1000Ng::getPrintableNetworkIdAndShortAddress(msg);
  Serial.print("Network ID & Device Address: ");
  Serial.println(msg);
  DW1000Ng::getPrintableDeviceMode(msg);
  Serial.print("Device mode: ");
  Serial.println(msg);
  // attach callback for (successfully) sent and received messages
  DW1000Ng::attachSentHandler(handleSent);
  DW1000Ng::attachReceivedHandler(handleReceived);
  // anchor starts in receiving mode, awaiting a ranging poll message

  receiver();
  noteActivity();
  // for first time ranging frequency computation
  rangingCountPeriod = millis();
}

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

void reconnect() {
  // MQTT 브로커에 연결 시도
  while (!client.connected()) {
    //Serial.print("Attempting MQTT connection...");
    if (client.connect("ESP32Client")) {
      //Serial.println("connected");
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void noteActivity() {
  // update activity timestamp, so that we do not reach "resetPeriod"
  lastActivity = millis();
}

void resetInactive() {
  // anchor listens for POLL
  expectedMsgId = POLL;
  receiver();
  noteActivity();
}

void handleSent() {
  // status change on sent success
  sentAck = true;
}

void handleReceived() {
  // status change on received success
  receivedAck = true;
}

void transmitPollAck() {
  data[0] = POLL_ACK;
  DW1000Ng::setTransmitData(data, LEN_DATA);
  DW1000Ng::startTransmit();
}

void transmitRangeReport(float curRange) {
  data[0] = RANGE_REPORT;
  // write final ranging result
  memcpy(data + 1, &curRange, 4);
  DW1000Ng::setTransmitData(data, LEN_DATA);
  DW1000Ng::startTransmit();
}

void transmitRangeFailed() {
  data[0] = RANGE_FAILED;
  DW1000Ng::setTransmitData(data, LEN_DATA);
  DW1000Ng::startTransmit();
}

void receiver() {
  DW1000Ng::forceTRxOff();
  // so we don't need to restart the receiver manually
  DW1000Ng::startReceive();
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();  // MQTT 클라이언트의 연결 상태 유지 및 처리

  int32_t curMillis = millis();
  unsigned long currentMillis = millis();

  // 현재 시간을 600ms(3개의 슬롯 주기)로 나눈 나머지를 계산하여 각 Anchor 할당
  int currentSlot = (currentMillis / SLOT_DURATION_MS) % NUM_ANCHORS;

  // 현재 시간의 초에 따라 Anchor C 활성화
  if (currentSlot == ANCHOR_C_SLOT) {
    if (!sentAck && !receivedAck) {
      // check if inactive
      if (curMillis - lastActivity > resetPeriod) {
        resetInactive();
      }
      return;
    }
    // continue on any success confirmation
    if (sentAck) {
      sentAck = false;
      byte msgId = data[0];
      if (msgId == POLL_ACK) {
        timePollAckSent = DW1000Ng::getTransmitTimestamp();
        noteActivity();
      }
      DW1000Ng::startReceive();
    }
    if (receivedAck) {
      receivedAck = false;
      // get message and parse
      DW1000Ng::getReceivedData(data, LEN_DATA);
      byte msgId = data[0];
      if (msgId != expectedMsgId) {
        // unexpected message, start over again (except if already POLL)
        protocolFailed = true;
      }
      if (msgId == POLL) {
        // on POLL we (re-)start, so no protocol failure
        protocolFailed = false;
        timePollReceived = DW1000Ng::getReceiveTimestamp();
        expectedMsgId = RANGE;
        transmitPollAck();
        noteActivity();
      } else if (msgId == RANGE) {
        timeRangeReceived = DW1000Ng::getReceiveTimestamp();
        expectedMsgId = POLL;
        if (!protocolFailed) {
          timePollSent = DW1000NgUtils::bytesAsValue(data + 1, LENGTH_TIMESTAMP);
          timePollAckReceived = DW1000NgUtils::bytesAsValue(data + 6, LENGTH_TIMESTAMP);
          timeRangeSent = DW1000NgUtils::bytesAsValue(data + 11, LENGTH_TIMESTAMP);
          // (re-)compute range as two-way ranging is done
          double distance = DW1000NgRanging::computeRangeAsymmetric(timePollSent,
                                                                    timePollReceived,
                                                                    timePollAckSent,
                                                                    timePollAckReceived,
                                                                    timeRangeSent,
                                                                    timeRangeReceived);
          /* Apply simple bias correction */
          distance = DW1000NgRanging::correctRange(distance);

          // Serial 출력 정보
          String rangeString = "Range: ";
          rangeString += distance;
          rangeString += " m";
          rangeString += "\t RX power: ";
          rangeString += DW1000Ng::getReceivePower();
          rangeString += " dBm";
          rangeString += "\t Sampling: ";
          rangeString += samplingRate;
          rangeString += " Hz";
          Serial.println(rangeString);

          if (distance <= 13 && distance >= 0) {
            // MQTT로 데이터 전송
            char msg[50];
            snprintf(msg, sizeof(msg), "%f", distance);
            if (client.publish(mqtt_topic, msg)) {
              Serial.println("MQTT publish success");
            } else {
              Serial.println("MQTT publish failed");
            }
          }


          // 전송 성공 여부
          transmitRangeReport(distance * DISTANCE_OF_RADIO_INV);
          successRangingCount++;
          if (curMillis - rangingCountPeriod > 1000) {
            samplingRate = (1000.0f * successRangingCount) / (curMillis - rangingCountPeriod);
            rangingCountPeriod = curMillis;
            successRangingCount = 0;
          }

        } else {
          transmitRangeFailed();
        }
        noteActivity();
      }
    }
  }
}
