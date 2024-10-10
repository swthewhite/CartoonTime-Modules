/**
 * @file Tag.ino
 * @brief Decawave DW1000Ng를 사용한 태그의 거리 측정 시스템.
 *        이 코드는 양방향 거리 측정을 수행하며, POLL 및 RANGE 메시지를 사용하여 앵커와 통신합니다.
 */

#include <DW1000Ng.hpp>
#include <DW1000NgUtils.hpp>
#include <DW1000NgTime.hpp>
#include <DW1000NgConstants.hpp>

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
volatile byte expectedMsgId = POLL_ACK; ///< 예상되는 메시지 ID

// 메시지 송수신 상태
volatile boolean sentAck = false;    ///< 메시지 전송 완료 여부
volatile boolean receivedAck = false; ///< 메시지 수신 완료 여부

// 거리 측정에 필요한 타임스탬프
uint64_t timePollSent;               ///< POLL 메시지 전송 시간
uint64_t timePollAckReceived;        ///< POLL_ACK 메시지 수신 시간
uint64_t timeRangeSent;              ///< RANGE 메시지 전송 시간

// 데이터 버퍼
#define LEN_DATA 16        ///< 데이터 버퍼 길이
byte data[LEN_DATA];       ///< 메시지 데이터를 저장할 버퍼

// 감시 타이머 및 재설정 주기
uint32_t lastActivity;     ///< 마지막 활동 시간
uint32_t resetPeriod = 250; ///< 재설정 주기 (밀리초)

// 응답 지연 시간
uint16_t replyDelayTimeUS = 3000; ///< 응답 지연 시간 (마이크로초)

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
 *        DW1000 모듈을 초기화하고 기본 설정을 적용하며, POLL 메시지를 전송하여 앵커와 통신을 시작합니다.
 */
void setup() {
    Serial.begin(115200);
    Serial.println(F("### DW1000Ng-arduino-ranging-tag ###"));
    
    // DW1000 모듈 초기화
    DW1000Ng::initialize(PIN_SS, PIN_IRQ, PIN_RST);
    Serial.println("DW1000Ng initialized ...");

    // DW1000 기본 설정 적용
    DW1000Ng::applyConfiguration(DEFAULT_CONFIG);
    DW1000Ng::applyInterruptConfiguration(DEFAULT_INTERRUPT_CONFIG);

    // 네트워크 ID 및 안테나 지연 설정
    DW1000Ng::setNetworkId(10);
    DW1000Ng::setAntennaDelay(16436);

    Serial.println(F("Committed configuration ..."));

    // 디버그용 정보 출력
    char msg[128];
    DW1000Ng::getPrintableDeviceIdentifier(msg);
    Serial.print("Device ID: "); Serial.println(msg);
    DW1000Ng::getPrintableExtendedUniqueIdentifier(msg);
    Serial.print("Unique ID: "); Serial.println(msg);
    DW1000Ng::getPrintableNetworkIdAndShortAddress(msg);
    Serial.print("Network ID & Device Address: "); Serial.println(msg);
    DW1000Ng::getPrintableDeviceMode(msg);
    Serial.print("Device mode: "); Serial.println(msg);

    // 메시지 송수신 핸들러 설정
    DW1000Ng::attachSentHandler(handleSent);
    DW1000Ng::attachReceivedHandler(handleReceived);

    // POLL 메시지 전송 시작
    transmitPoll();
    noteActivity();
}

/**
 * @brief 마지막 활동 시간을 기록하는 함수
 *        마지막 활동 시간을 기록하여 장치가 재설정되지 않도록 합니다.
 */
void noteActivity() {
    lastActivity = millis();
}

/**
 * @brief 일정 시간 동안 비활성 상태일 경우 재설정하는 함수
 *        태그가 POLL 메시지를 전송하고, POLL_ACK를 대기합니다.
 */
void resetInactive() {
    expectedMsgId = POLL_ACK;
    DW1000Ng::forceTRxOff();
    transmitPoll();
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
 * @brief POLL 메시지를 전송하는 함수
 *        태그가 앵커로 POLL 메시지를 전송하여 거리 측정을 시작합니다.
 */
void transmitPoll() {
    data[0] = POLL;
    DW1000Ng::setTransmitData(data, LEN_DATA);
    DW1000Ng::startTransmit();
}

/**
 * @brief RANGE 메시지를 전송하는 함수
 *        POLL_ACK 메시지를 받은 후 거리 측정을 위해 RANGE 메시지를 전송합니다.
 */
void transmitRange() {
    data[0] = RANGE;

    // 미래 시간 계산
    byte futureTimeBytes[LENGTH_TIMESTAMP];

    timeRangeSent = DW1000Ng::getSystemTimestamp();
    timeRangeSent += DW1000NgTime::microsecondsToUWBTime(replyDelayTimeUS);
    DW1000NgUtils::writeValueToBytes(futureTimeBytes, timeRangeSent, LENGTH_TIMESTAMP);
    DW1000Ng::setDelayedTRX(futureTimeBytes);
    timeRangeSent += DW1000Ng::getTxAntennaDelay();

    // 타임스탬프 전송
    DW1000NgUtils::writeValueToBytes(data + 1, timePollSent, LENGTH_TIMESTAMP);
    DW1000NgUtils::writeValueToBytes(data + 6, timePollAckReceived, LENGTH_TIMESTAMP);
    DW1000NgUtils::writeValueToBytes(data + 11, timeRangeSent, LENGTH_TIMESTAMP);
    DW1000Ng::setTransmitData(data, LEN_DATA);
    DW1000Ng::startTransmit(TransmitMode::DELAYED);
}

/**
 * @brief 주 실행 루프
 *        메시지 전송 및 수신을 관리하고, 거리 측정을 수행합니다.
 */
void loop() {
    if (!sentAck && !receivedAck) {
        // 비활성 상태 확인
        if (millis() - lastActivity > resetPeriod) {
            resetInactive();
        }
        return;
    }

    // 메시지 전송 성공 처리
    if (sentAck) {
        sentAck = false;
        DW1000Ng::startReceive();
    }

    // 메시지 수신 성공 처리
    if (receivedAck) {
        receivedAck = false;

        // 메시지 처리 및 파싱
        DW1000Ng::getReceivedData(data, LEN_DATA);
        byte msgId = data[0];

        // 예상되지 않은 메시지 처리
        if (msgId != expectedMsgId) {
            expectedMsgId = POLL_ACK;
            transmitPoll();
            return;
        }

        // POLL_ACK 메시지 수신 처리
        if (msgId == POLL_ACK) {
            timePollSent = DW1000Ng::getTransmitTimestamp();
            timePollAckReceived = DW1000Ng::getReceiveTimestamp();
            expectedMsgId = RANGE_REPORT;
            transmitRange();
            noteActivity();
        }

        // RANGE_REPORT 메시지 수신 처리
        else if (msgId == RANGE_REPORT) {
            expectedMsgId = POLL_ACK;
            float curRange;
            memcpy(&curRange, data + 1, 4);
            transmitPoll();
            noteActivity();
        }

        // RANGE_FAILED 메시지 처리
        else if (msgId == RANGE_FAILED) {
            expectedMsgId = POLL_ACK;
            transmitPoll();
            noteActivity();
        }
    }
}
