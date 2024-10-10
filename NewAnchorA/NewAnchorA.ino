/**
 * @file AnchorA.ino
 * @brief Decawave DW1000Ng를 사용한 앵커 A의 거리 측정 시스템. 
 *        이 코드는 WiFi, MQTT, NTP 서버와 연동되며, 타임 슬롯 방식으로
 *        UWB 기반의 앵커 시스템을 구현합니다.
 */

#include <WiFi.h>
#include <time.h>
#include <PubSubClient.h>

#include <DW1000Ng.hpp>
#include <DW1000NgUtils.hpp>
#include <DW1000NgRanging.hpp>

#include "moduleConfig.h"

// NTP 서버 설정 (기본적으로 pool.ntp.org 사용)
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0;      ///< GMT +0 (UTC 기준)
const int daylightOffset_sec = 0;  ///< Daylight Saving 시간 없음

// MQTT 토픽 설정
const char* mqtt_topic = "myRange/A";

WiFiClient espClient;
PubSubClient client(espClient);

const int SLOT_DURATION_MS = 200;  ///< 타임 슬롯 길이 (200ms)
const int NUM_ANCHORS = 3;         ///< 총 3개의 Anchor 사용
const int ANCHOR_A_SLOT = 0;       ///< Anchor A 동작 시간 (0~200ms)
const int ANCHOR_B_SLOT = 1;       ///< Anchor B 동작 시간 (200~400ms)
const int ANCHOR_C_SLOT = 2;       ///< Anchor C 동작 시간 (400~600ms)

// SPI 연결 핀 설정
const uint8_t PIN_SCK = 18;   ///< SPI 클럭 핀
const uint8_t PIN_MOSI = 23;  ///< SPI 데이터 출력 핀
const uint8_t PIN_MISO = 19;  ///< SPI 데이터 입력 핀
const uint8_t PIN_SS = 4;     ///< SPI 슬레이브 선택 핀
const uint8_t PIN_RST = 15;   ///< 모듈 리셋 핀
const uint8_t PIN_IRQ = 17;   ///< 인터럽트 핀

// 거리 측정 프로토콜에서 사용되는 메시지 정의
// TODO enum으로 대체할 예정
#define POLL 0           ///< 폴링 메시지
#define POLL_ACK 1       ///< 폴링 응답 메시지
#define RANGE 2          ///< 거리 측정 메시지
#define RANGE_REPORT 3   ///< 거리 측정 결과 메시지
#define RANGE_FAILED 255 ///< 거리 측정 실패 메시지

// 메시지 흐름 상태
volatile byte expectedMsgId = POLL; ///< 예상되는 메시지 ID

// 메시지 송수신 상태
volatile boolean sentAck = false;    ///< 메시지 전송 완료 여부
volatile boolean receivedAck = false; ///< 메시지 수신 완료 여부

// 프로토콜 에러 상태
boolean protocolFailed = false; ///< 프로토콜 오류 상태

// 거리 측정에 필요한 타임스탬프
uint64_t timePollSent;
uint64_t timePollReceived;
uint64_t timePollAckSent;
uint64_t timePollAckReceived;
uint64_t timeRangeSent;
uint64_t timeRangeReceived;

uint64_t timeComputedRange; ///< 계산된 마지막 거리

// 데이터 버퍼
#define LEN_DATA 16        ///< 데이터 버퍼 길이
byte data[LEN_DATA];       ///< 메시지 데이터를 저장할 버퍼

// 감시 타이머 및 재설정 주기
uint32_t lastActivity;     ///< 마지막 활동 시간
uint32_t resetPeriod = 250; ///< 재설정 주기 (밀리초)

// 응답 지연 시간
uint16_t replyDelayTimeUS = 3000; ///< 응답 지연 시간 (마이크로초)

// 거리 측정 성공 횟수 카운터 (초당)
uint16_t successRangingCount = 0; ///< 거리 측정 성공 횟수
uint32_t rangingCountPeriod = 0;  ///< 거리 측정 주기
float samplingRate = 0;           ///< 샘플링 속도 (Hz)

// DW1000 기본 설정
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

// DW1000 인터럽트 설정
interrupt_configuration_t DEFAULT_INTERRUPT_CONFIG = {
  true,
  true,
  true,
  false,
  true
};

/**
 * @brief 시스템 설정 및 초기화
 *        WiFi 연결, NTP 서버 설정, MQTT 브로커 연결, DW1000 초기화를 수행합니다.
 */
void setup() {
  Serial.begin(115200);
  delay(1000);

  // WiFi 연결 설정
  setup_wifi();

  // NTP 서버와 시간 동기화
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // MQTT 브로커 연결
  client.setServer(MQTT_SERVER, MQTT_PORT);

  Serial.println(F("### DW1000Ng-arduino-ranging-anchorA ###"));
  // DW1000 모듈 초기화
  DW1000Ng::initialize(PIN_SS, PIN_IRQ, PIN_RST);
  Serial.println(F("DW1000Ng initialized ..."));

  // DW1000 기본 설정 적용
  DW1000Ng::applyConfiguration(DEFAULT_CONFIG);
  DW1000Ng::applyInterruptConfiguration(DEFAULT_INTERRUPT_CONFIG);

  // 앵커의 장치 주소 설정
  DW1000Ng::setDeviceAddress(1);

  // 안테나 지연 설정
  DW1000Ng::setAntennaDelay(16436);

  Serial.println(F("Committed configuration ..."));
  
  // 디버그용 정보 출력
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

  // 메시지 송수신 핸들러 설정
  DW1000Ng::attachSentHandler(handleSent);
  DW1000Ng::attachReceivedHandler(handleReceived);

  // 수신 모드 시작 (POLL 메시지 대기)
  receiver();
  noteActivity();

  // 거리 측정 주기 초기화
  rangingCountPeriod = millis();
}

/**
 * @brief WiFi 설정 및 연결 함수
 *        WiFi SSID 및 비밀번호를 사용하여 연결합니다.
 */
void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // WiFi 연결 대기
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}

/**
 * @brief MQTT 브로커에 다시 연결하는 함수
 *        연결이 끊어졌을 경우 재연결 시도를 합니다.
 */
void reconnect() {
  while (!client.connected()) {
    if (client.connect("ESP32Client")) {
      // 연결 성공
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

/**
 * @brief 최근 활동 시간을 기록하는 함수
 *        마지막 활동 시간을 기록하여 장치가 재설정되지 않도록 합니다.
 */
void noteActivity() {
  lastActivity = millis();
}

/**
 * @brief 일정 시간 동안 비활성 상태일 경우 재설정하는 함수
 *        앵커가 수신 대기 모드로 전환됩니다.
 */
void resetInactive() {
  expectedMsgId = POLL;
  receiver();
  noteActivity();
}

/**
 * @brief 메시지가 성공적으로 전송되었을 때 호출되는 함수
 */
void handleSent() {
  sentAck = true;
}

/**
 * @brief 메시지가 성공적으로 수신되었을 때 호출되는 함수
 */
void handleReceived() {
  receivedAck = true;
}

/**
 * @brief POLL_ACK 메시지를 전송하는 함수
 */
void transmitPollAck() {
  data[0] = POLL_ACK;
  DW1000Ng::setTransmitData(data, LEN_DATA);
  DW1000Ng::startTransmit();
}

/**
 * @brief 거리 측정 결과 메시지를 전송하는 함수
 * @param curRange 계산된 거리 값
 */
void transmitRangeReport(float curRange) {
  data[0] = RANGE_REPORT;
  memcpy(data + 1, &curRange, 4);
  DW1000Ng::setTransmitData(data, LEN_DATA);
  DW1000Ng::startTransmit();
}

/**
 * @brief 거리 측정 실패 메시지를 전송하는 함수
 */
void transmitRangeFailed() {
  data[0] = RANGE_FAILED;
  DW1000Ng::setTransmitData(data, LEN_DATA);
  DW1000Ng::startTransmit();
}

/**
 * @brief 수신 모드를 시작하는 함수
 */
void receiver() {
  DW1000Ng::forceTRxOff();
  DW1000Ng::startReceive();
}

/**
 * @brief 주 실행 루프
 *        MQTT 상태를 확인하고, 앵커 A에서 타임 슬롯을 기반으로 거리 측정 작업을 수행합니다.
 */
void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  int32_t curMillis = millis();
  unsigned long currentMillis = millis();

  // 현재 시간을 기준으로 각 앵커에게 타임 슬롯 할당
  int currentSlot = (currentMillis / SLOT_DURATION_MS) % NUM_ANCHORS;

  // 현재 시간의 초에 따라 Anchor A가 활성화됨
  if (currentSlot == ANCHOR_A_SLOT) {
    if (!sentAck && !receivedAck) {
      if (curMillis - lastActivity > resetPeriod) {
        resetInactive();
      }
      return;
    }
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
      DW1000Ng::getReceivedData(data, LEN_DATA);
      byte msgId = data[0];
      if (msgId != expectedMsgId) {
        protocolFailed = true;
      }
      if (msgId == POLL) {
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
          double distance = DW1000NgRanging::computeRangeAsymmetric(timePollSent,
                                                             timePollReceived,
                                                             timePollAckSent,
                                                             timePollAckReceived,
                                                             timeRangeSent,
                                                             timeRangeReceived);
          distance = DW1000NgRanging::correctRange(distance);

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
            char msg[50];
            snprintf(msg, sizeof(msg), "%f", distance);
            if (client.publish(mqtt_topic, msg)) {
              Serial.println("MQTT publish success");
            } else {
              Serial.println("MQTT publish failed");
            }
          }

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
