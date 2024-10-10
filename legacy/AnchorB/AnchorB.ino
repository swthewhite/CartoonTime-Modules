/*
   이 예제 코드는 퍼블릭 도메인에 속하거나 사용자의 선택에 따라 CC0 라이센스가 적용됩니다.

   해당 법률에 의해 요구되지 않는 한, 이 소프트웨어는 "있는 그대로(AS IS)" 제공되며, 
   어떠한 형태의 보증 또는 조건도 포함되지 않습니다.
*/

/* 
 * StandardRTLSAnchorB_TWR.ino
 * 
 * 이 코드는 두 방향 거리 측정 ISO/IEC 24730-62_2013 메시지를 사용하는 RTLS(실시간 위치 추적 시스템)에서 
 * 보조 앵커(Anchor B)의 역할을 수행하는 예제입니다.
 */

#include <DW1000Ng.hpp>              // DW1000 모듈 제어 라이브러리
#include <DW1000NgUtils.hpp>         // DW1000 관련 유틸리티 함수
#include <DW1000NgRanging.hpp>       // 거리 측정 관련 함수
#include <DW1000NgRTLS.hpp>          // RTLS 시스템 구현 관련 함수

// SPI 통신에 사용될 핀 설정
const uint8_t PIN_SCK = 18;          // SPI 클럭 핀
const uint8_t PIN_MOSI = 23;         // SPI 데이터 출력 핀
const uint8_t PIN_MISO = 19;         // SPI 데이터 입력 핀
const uint8_t PIN_SS = 4;            // SPI 슬레이브 선택 핀
const uint8_t PIN_RST = 15;          // 모듈 리셋 핀
const uint8_t PIN_IRQ = 17;          // 인터럽트 핀

// 64비트 장치 식별자 (EUI)
const char EUI[] = "AA:BB:CC:DD:EE:FF:00:02";  // Anchor B의 고유 식별자

// 주 앵커(Anchor A)의 주소
byte main_anchor_address[] = {0x01, 0x00};

// 다음 앵커로 데이터를 넘기기 위한 변수 설정
uint16_t next_anchor = 3;

// 보조 앵커와 태그 간의 거리 측정 결과 저장 변수
double range_self;

// 기본 UWB 모듈 설정 (device_configuration_t 구조체 사용)
device_configuration_t DEFAULT_CONFIG = {
    false,                          // 확장된 프레임 길이 사용 안함
    true,                           // 자동 수신 재활성화
    true,                           // 스마트 전력 사용
    true,                           // CRC 프레임 체크 활성화
    false,                          // NLOS 모드 비활성화
    SFDMode::STANDARD_SFD,          // 표준 SFD 모드 사용
    Channel::CHANNEL_5,             // 채널 5 사용
    DataRate::RATE_850KBPS,         // 데이터 전송 속도 850KBPS 설정
    PulseFrequency::FREQ_16MHZ,     // 펄스 주파수 16MHz 설정
    PreambleLength::LEN_256,        // 프리앰블 길이 256
    PreambleCode::CODE_3            // 프리앰블 코드 3 사용
};

// 프레임 필터링 설정 (frame_filtering_configuration_t 구조체 사용)
frame_filtering_configuration_t ANCHOR_FRAME_FILTER_CONFIG = {
    false,                          // 코디네이터 역할 비활성화
    false,                          // 비콘 프레임 비허용
    true,                           // 데이터 프레임 허용
    false,                          // ACK 프레임 비허용
    false,                          // MAC 명령 프레임 비허용
    false,                          // 예약된 필드 비허용
    false,                          // 예약된 필드 4 비허용
    false                           // 예약된 필드 5 비허용
};

// 초기화 함수: UWB 모듈 및 기본 설정 적용
void setup() {
    // 시리얼 통신을 시작하여 디버그 정보를 출력
    Serial.begin(115200);
    Serial.println(F("### arduino-DW1000Ng-ranging-anchor-B ###"));
    
    // DW1000 모듈 초기화 (ESP32/ESP8266 환경에 맞춰 초기화)
    #if defined(ESP8266)
    DW1000Ng::initializeNoInterrupt(PIN_SS);  // 인터럽트 없이 초기화 (ESP8266)
    #else
    DW1000Ng::initializeNoInterrupt(PIN_SS, PIN_RST);  // 인터럽트 없이 초기화 (ESP32)
    #endif
    Serial.println(F("DW1000Ng initialized ..."));

    // 기본 장치 설정 적용
    DW1000Ng::applyConfiguration(DEFAULT_CONFIG);

    // 프레임 필터링 활성화
    DW1000Ng::enableFrameFiltering(ANCHOR_FRAME_FILTER_CONFIG);
    
    // 장치의 고유 식별자(EUI) 설정
    DW1000Ng::setEUI(EUI);

    // 타임아웃 설정
    DW1000Ng::setPreambleDetectionTimeout(64);  // 프리앰블 검출 타임아웃 설정
    DW1000Ng::setSfdDetectionTimeout(273);      // SFD 검출 타임아웃 설정
    DW1000Ng::setReceiveFrameWaitTimeoutPeriod(5000);  // 프레임 대기 타임아웃 설정

    // 네트워크 ID 및 장치 주소 설정
    DW1000Ng::setNetworkId(RTLS_APP_ID);  // RTLS 네트워크 ID 설정
    DW1000Ng::setDeviceAddress(2);        // Anchor B의 주소 설정
	
    // 안테나 지연 시간 설정
    DW1000Ng::setAntennaDelay(16436);     // 안테나 지연 시간 설정
    
    Serial.println(F("Committed configuration ..."));

    // 디버그 정보 출력
    char msg[128];
    DW1000Ng::getPrintableDeviceIdentifier(msg);
    Serial.print("Device ID: "); Serial.println(msg);  // 장치 ID 출력
    DW1000Ng::getPrintableExtendedUniqueIdentifier(msg);
    Serial.print("Unique ID: "); Serial.println(msg);  // 고유 식별자 출력
    DW1000Ng::getPrintableNetworkIdAndShortAddress(msg);
    Serial.print("Network ID & Device Address: "); Serial.println(msg);  // 네트워크 정보 출력
    DW1000Ng::getPrintableDeviceMode(msg);
    Serial.print("Device mode: "); Serial.println(msg);  // 장치 모드 출력
}

// 거리 측정 결과를 주 앵커에 전송하는 함수
void transmitRangeReport() {
    // 거리 보고용 패킷을 생성
    byte rangingReport[] = {DATA, SHORT_SRC_AND_DEST, DW1000NgRTLS::increaseSequenceNumber(), 0,0, 0,0, 0,0, 0x60, 0,0 };
    
    // 네트워크 ID 및 송수신 주소 설정
    DW1000Ng::getNetworkId(&rangingReport[3]);  // 네트워크 ID
    memcpy(&rangingReport[5], main_anchor_address, 2);  // 주 앵커 주소
    DW1000Ng::getDeviceAddress(&rangingReport[7]);  // Anchor B의 주소
    
    // 거리 정보를 패킷에 추가 (거리 단위: mm)
    DW1000NgUtils::writeValueToBytes(&rangingReport[10], static_cast<uint16_t>((range_self * 1000)), 2);
    
    // 패킷을 전송
    DW1000Ng::setTransmitData(rangingReport, sizeof(rangingReport));
    DW1000Ng::startTransmit();  // 전송 시작
}

// 메인 루프 함수: 거리 측정 및 보고
void loop() {     
    // 주 앵커와의 거리 측정 요청 처리
    RangeAcceptResult result = DW1000NgRTLS::anchorRangeAccept(NextActivity::RANGING_CONFIRM, next_anchor);
    
    // 거리 측정이 성공한 경우
    if(result.success) {
        delay(2);  // 하드웨어에 따른 지연 시간 설정
        range_self = result.range;  // 측정된 거리 저장

        // 측정된 거리 정보를 주 앵커에 전송
        transmitRangeReport();

        // 디버그용으로 거리 및 신호 강도 출력
        String rangeString = "Range: "; rangeString += range_self; rangeString += " m";
        rangeString += "\t RX power: "; rangeString += DW1000Ng::getReceivePower(); rangeString += " dBm";
        Serial.println(rangeString);  // 시리얼 모니터에 출력
    }
}
