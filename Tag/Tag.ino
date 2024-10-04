/*
   이 코드 예시는 퍼블릭 도메인에 속하거나 사용자의 선택에 따라 CC0 라이센스가 적용됩니다.

   해당 법률에 의해 요구되지 않는 한, 이 소프트웨어는 "있는 그대로(AS IS)" 제공되며, 
   어떠한 형태의 보증 또는 조건도 포함되지 않습니다.
*/

/*
 * StandardRTLSTag_TWR.ino
 *
 * 이 코드는 두 개의 장치 간 두 웨이 레이징(두 방향 거리 측정)을 사용하여 
 * ISO/IEC 24730-62_2013 메시지 기반으로 RTLS(실시간 위치 추적 시스템)의 태그를 구현하는 예제입니다.
 */

#include <DW1000Ng.hpp>              // DW1000 라이브러리
#include <DW1000NgUtils.hpp>         // DW1000 유틸리티 함수
#include <DW1000NgTime.hpp>          // 시간 관련 유틸리티
#include <DW1000NgConstants.hpp>     // DW1000 상수들
#include <DW1000NgRanging.hpp>       // 거리 측정 관련 함수
#include <DW1000NgRTLS.hpp>          // 실시간 위치 추적 시스템(RTLS) 관련 함수

// 하드웨어 핀 설정
const uint8_t PIN_SCK = 18;          // SPI 클럭 핀
const uint8_t PIN_MOSI = 23;         // SPI 데이터 출력 핀
const uint8_t PIN_MISO = 19;         // SPI 데이터 입력 핀
const uint8_t PIN_SS = 4;            // SPI 슬레이브 선택 핀
const uint8_t PIN_RST = 15;          // 모듈 리셋 핀
const uint8_t PIN_IRQ = 17;          // 인터럽트 핀

// 장치 고유 식별자(EUI) 설정 (64비트)
const char EUI[] = "AA:BB:CC:DD:EE:FF:00:00";  // 장치의 고유 ID

// 깜빡임 주기를 나타내는 변수. 초기에 200ms로 설정.
volatile uint32_t blink_rate = 200;

// 기본 장치 설정 (UWB 모듈 초기화에 사용)
device_configuration_t DEFAULT_CONFIG = {
    false,                          // 확장된 프레임 길이 사용 안함
    true,                           // 수신 자동 재활성화
    true,                           // 스마트 전력 관리 사용
    true,                           // CRC 프레임 체크 활성화
    false,                          // NLOS 모드 비활성화
    SFDMode::STANDARD_SFD,          // 표준 SFD 모드 사용
    Channel::CHANNEL_5,             // UWB 채널 5 사용
    DataRate::RATE_850KBPS,         // 데이터 전송 속도 850KBPS 설정
    PulseFrequency::FREQ_16MHZ,     // 펄스 주파수 16MHz 설정
    PreambleLength::LEN_256,        // 프리앰블 길이 256
    PreambleCode::CODE_3            // 프리앰블 코드 3 사용
};

// 프레임 필터링 설정 (필요한 메시지만 수신)
frame_filtering_configuration_t TAG_FRAME_FILTER_CONFIG = {
    false,                          // 코디네이터 모드 사용 안함
    false,                          // 비콘 필터 허용 안함
    true,                           // 데이터 프레임 허용
    false,                          // 확인 프레임 허용 안함
    false,                          // MAC 명령어 허용 안함
    false,                          // 예약된 필드 허용 안함
    false,                          // 추가 예약 필드 4 허용 안함
    false                           // 추가 예약 필드 5 허용 안함
};

// 슬립 모드 설정 (전력 절약 기능)
sleep_configuration_t SLEEP_CONFIG = {
    false,  // 깨어날 때 ADC 실행 안함
    false,  // 깨어날 때 수신 모드 안함
    false,  // 깨어날 때 EUI 로드 안함
    true,   // 깨어날 때 L64 매개변수 로드
    true,   // 슬립 모드 유지
    true,   // 슬립 모드 활성화
    false,  // 웨이크업 핀 활성화 안함
    true    // SPI 웨이크업 활성화
};

// 초기화 함수: 설정 및 기본 설정 적용
void setup() {
    // 시리얼 모니터 설정
    Serial.begin(115200);           // 시리얼 통신 속도 설정
    Serial.println(F("### DW1000Ng-arduino-ranging-tag ###"));  // 초기 메시지 출력
    
    // DW1000 모듈 초기화
    #if defined(ESP8266)
    Serial.println(F("ESP8266 init DW1000"));
    DW1000Ng::initializeNoInterrupt(PIN_SS);  // 인터럽트 없는 초기화 (ESP8266용)
    #else
    Serial.println(F("ESP32 init DW1000"));
    DW1000Ng::initializeNoInterrupt(PIN_SS, PIN_RST);  // 인터럽트 없는 초기화 (ESP32용)
    #endif
    Serial.println("DW1000Ng initialized ...");  // 초기화 완료 메시지 출력

    // 기본 장치 설정 적용
    DW1000Ng::applyConfiguration(DEFAULT_CONFIG);  // 장치 설정 적용
    DW1000Ng::enableFrameFiltering(TAG_FRAME_FILTER_CONFIG);  // 프레임 필터링 활성화
    
    // 장치 고유 식별자 설정
    DW1000Ng::setEUI(EUI);  // EUI 설정
    
    // 네트워크 ID 및 안테나 지연 시간 설정
    DW1000Ng::setNetworkId(RTLS_APP_ID);  // 네트워크 ID 설정
    DW1000Ng::setAntennaDelay(16436);  // 안테나 지연 시간 설정 (칼리브레이션 필요)

    // 슬립 모드 설정 적용
    DW1000Ng::applySleepConfiguration(SLEEP_CONFIG);  // 슬립 모드 설정 적용

    // 다양한 타이밍 설정
    DW1000Ng::setPreambleDetectionTimeout(15);  // 프리앰블 검출 타임아웃 설정
    DW1000Ng::setSfdDetectionTimeout(273);  // SFD 검출 타임아웃 설정
    DW1000Ng::setReceiveFrameWaitTimeoutPeriod(2000);  // 프레임 대기 시간 설정
    
    // 디버그용 출력 (장치 정보 및 설정)
    char msg[128];
    DW1000Ng::getPrintableDeviceIdentifier(msg);  // 장치 ID 가져오기
    Serial.print("Device ID: "); Serial.println(msg);  // 장치 ID 출력
    DW1000Ng::getPrintableExtendedUniqueIdentifier(msg);  // 고유 식별자 가져오기
    Serial.print("Unique ID: "); Serial.println(msg);  // 고유 식별자 출력
    DW1000Ng::getPrintableNetworkIdAndShortAddress(msg);  // 네트워크 ID 및 짧은 주소 가져오기
    Serial.print("Network ID & Device Address: "); Serial.println(msg);  // 네트워크 정보 출력
    DW1000Ng::getPrintableDeviceMode(msg);  // 장치 모드 가져오기
    Serial.print("Device mode: "); Serial.println(msg);  // 장치 모드 출력
}

// 메인 루프 함수: 슬립 모드 및 거리 측정 반복 수행
void loop() {
    // 전력 절약을 위해 슬립 모드 진입
    DW1000Ng::deepSleep();  // 슬립 모드로 전환
    delay(blink_rate);  // 깜빡임 주기만큼 지연 (거리 측정 주기)

    // SPI를 통해 슬립 모드 해제
    DW1000Ng::spiWakeup();  // SPI 통신으로 장치 깨우기
    DW1000Ng::setEUI(EUI);  // EUI 재설정 (슬립 모드에서 깨어났을 때)

    // TWR(두 방향 거리 측정) 방식으로 거리 측정
    RangeInfrastructureResult res = DW1000NgRTLS::tagTwrLocalize(1500);  // 거리 측정 요청
    if(res.success)  // 거리 측정이 성공하면
        blink_rate = res.new_blink_rate;  // 새로운 깜빡임 주기 설정 (거리 기반으로 조정)
}
