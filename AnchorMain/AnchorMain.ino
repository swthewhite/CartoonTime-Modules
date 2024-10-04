/*
   이 예제 코드는 퍼블릭 도메인에 속하거나 사용자의 선택에 따라 CC0 라이센스가 적용됩니다.

   해당 법률에 의해 요구되지 않는 한, 이 소프트웨어는 "있는 그대로(AS IS)" 제공되며, 
   어떠한 형태의 보증 또는 조건도 포함되지 않습니다.
*/

/* 
 * StandardRTLSAnchorMain_TWR.ino
 * 
 * 이 코드는 두 방향 거리 측정 ISO/IEC 24730-62_2013 메시지를 사용하는 RTLS(실시간 위치 추적 시스템)에서 
 * 마스터 앵커의 역할을 하는 예제입니다.
 */

#include <DW1000Ng.hpp>             // DW1000 모듈 제어를 위한 라이브러리
#include <DW1000NgUtils.hpp>        // DW1000 관련 유틸리티 함수
#include <DW1000NgRanging.hpp>      // 거리 측정을 위한 함수
#include <DW1000NgRTLS.hpp>         // 실시간 위치 추적 시스템(RTLS) 관련 함수

// 앵커 위치를 저장할 구조체 정의
typedef struct Position {
    double x;   // X좌표
    double y;   // Y좌표
} Position;

// 하드웨어 핀 설정 (SPI 통신에 사용)
const uint8_t PIN_SCK = 18;
const uint8_t PIN_MOSI = 23;
const uint8_t PIN_MISO = 19;
const uint8_t PIN_SS = 4;
const uint8_t PIN_RST = 15;
const uint8_t PIN_IRQ = 17;

// 고유 식별자 (64비트 장치 식별자)
const char EUI[] = "AA:BB:CC:DD:EE:FF:00:01";

// 앵커 및 태그의 좌표 설정
Position position_self = {0, 0};          // 현재 앵커의 위치 (0, 0)
Position position_B = {5, 0};           // 두 번째 앵커의 위치 (5, 0)
Position position_C = {5, 5};        // 세 번째 앵커의 위치 (5, 5)

// 각 앵커와의 거리 변수
double range_self;    // 현재 앵커와 태그 간의 거리
double range_B;       // 앵커 B와 태그 간의 거리
double range_C;       // 앵커 C와 태그 간의 거리

// 앵커 B에서 수신한 데이터를 처리했는지 여부
boolean received_B = false;

// 태그 및 다른 앵커의 식별자
byte target_eui[8];               // 목표 태그의 EUI
byte tag_shortAddress[] = {0x05, 0x00};   // 태그의 짧은 주소
byte anchor_b[] = {0x02, 0x00};    // 앵커 B의 주소
uint16_t next_anchor = 2;          // 다음 앵커 번호
byte anchor_c[] = {0x03, 0x00};    // 앵커 C의 주소

// 기본 UWB 모듈 설정
device_configuration_t DEFAULT_CONFIG = {
    false,                             // 확장된 프레임 길이 비활성화
    true,                              // 자동 수신 재활성화
    true,                              // 스마트 전력 사용
    true,                              // 프레임 체크(CRC) 활성화
    false,                             // NLOS 모드 비활성화
    SFDMode::STANDARD_SFD,             // 표준 SFD 모드 사용
    Channel::CHANNEL_5,                // 채널 5 사용
    DataRate::RATE_850KBPS,            // 데이터 속도 850KBPS
    PulseFrequency::FREQ_16MHZ,        // 펄스 주파수 16MHz
    PreambleLength::LEN_256,           // 프리앰블 길이 256
    PreambleCode::CODE_3               // 프리앰블 코드 3 사용
};

// 프레임 필터링 설정 (블링크 프레임 허용)
frame_filtering_configuration_t ANCHOR_FRAME_FILTER_CONFIG = {
    false,  // 코디네이터 역할 비활성화
    false,  // 비콘 프레임 비허용
    true,   // 데이터 프레임 허용
    false,  // ACK 프레임 비허용
    false,  // MAC 명령 프레임 비허용
    false,  // 예약된 필드 비허용
    false,  // 예약된 필드 비허용 (4)
    true    // 블링크 프레임 허용
};

// 초기화 함수
void setup() {
    // 시리얼 통신 초기화
    Serial.begin(115200);
    Serial.println(F("### DW1000Ng-arduino-ranging-anchorMain ###"));
    
    // DW1000 모듈 초기화
    #if defined(ESP8266)
    Serial.println(F("ESP8266 init DW1000"));
    DW1000Ng::initializeNoInterrupt(PIN_SS);  // 인터럽트 없는 초기화 (ESP8266용)
    #else
    Serial.println(F("ESP32 init DW1000"));
    DW1000Ng::initializeNoInterrupt(PIN_SS, PIN_RST);  // 인터럽트 없는 초기화 (ESP32용)
    #endif
    Serial.println(F("DW1000Ng initialized ..."));

    // 기본 장치 설정 적용
    DW1000Ng::applyConfiguration(DEFAULT_CONFIG);

    // 프레임 필터링 활성화
    DW1000Ng::enableFrameFiltering(ANCHOR_FRAME_FILTER_CONFIG);
    
    // 장치의 고유 식별자(EUI) 설정
    DW1000Ng::setEUI(EUI);

    // 타임아웃 및 네트워크 설정
    DW1000Ng::setPreambleDetectionTimeout(64);  // 프리앰블 검출 타임아웃
    DW1000Ng::setSfdDetectionTimeout(273);      // SFD 검출 타임아웃
    DW1000Ng::setReceiveFrameWaitTimeoutPeriod(5000);  // 프레임 대기 타임아웃

    DW1000Ng::setNetworkId(RTLS_APP_ID);  // 네트워크 ID 설정
    DW1000Ng::setDeviceAddress(1);        // 앵커의 주소 설정
	
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

/* 트릴레테이션(trilateration) 방식으로 태그의 위치 계산
   자세한 설명은 https://math.stackexchange.com/questions/884807 */
void calculatePosition(double &x, double &y) {

    // 앵커와 태그가 동일한 평면에 있다고 가정
    double A = ( (-2 * position_self.x) + (2 * position_B.x) );
    double B = ( (-2 * position_self.y) + (2 * position_B.y) );
    double C = (range_self * range_self) - (range_B * range_B) - (position_self.x * position_self.x) + (position_B.x * position_B.x) - (position_self.y * position_self.y) + (position_B.y * position_B.y);
    double D = ( (-2 * position_B.x) + (2 * position_C.x) );
    double E = ( (-2 * position_B.y) + (2 * position_C.y) );
    double F = (range_B * range_B) - (range_C * range_C) - (position_B.x * position_B.x) + (position_C.x * position_C.x) - (position_B.y * position_B.y) + (position_C.y * position_C.y);

    // X, Y 좌표 계산
    x = (C * E - F * B) / (E * A - B * D);
    y = (C * D - A * F) / (B * D - A * E);
}

// 메인 루프 함수
void loop() {
    // 프레임을 수신하는 경우
    if(DW1000NgRTLS::receiveFrame()) {
        size_t recv_len = DW1000Ng::getReceivedDataLength();  // 수신된 데이터 길이 확인
        byte recv_data[recv_len];  // 수신된 데이터를 저장할 배열
        DW1000Ng::getReceivedData(recv_data, recv_len);  // 수신된 데이터 가져오기

        // 블링크 프레임을 수신한 경우
        if(recv_data[0] == BLINK) {
            // 태그로부터 거리 측정을 시작 (블링크 프레임의 데이터를 바탕으로)
            DW1000NgRTLS::transmitRangingInitiation(&recv_data[2], tag_shortAddress);
            DW1000NgRTLS::waitForTransmission();  // 전송 완료 대기

            // 앵커와 태그 간 거리 측정
            RangeAcceptResult result = DW1000NgRTLS::anchorRangeAccept(NextActivity::RANGING_CONFIRM, next_anchor);
            if(!result.success) return;  // 실패 시 종료
            range_self = result.range;  // 현재 앵커와 태그 간의 거리 저장

        // 거리 측정 결과를 수신한 경우
        } else if(recv_data[9] == 0x60) {
            double range = static_cast<double>(DW1000NgUtils::bytesAsValue(&recv_data[10],2) / 1000.0);  // 거리를 미터 단위로 변환
            if(received_B == false && recv_data[7] == anchor_b[0] && recv_data[8] == anchor_b[1]) {
                range_B = range;  // 앵커 B와의 거리 저장
                received_B = true;
            } else if(received_B == true && recv_data[7] == anchor_c[0] && recv_data[8] == anchor_c[1]) {
                range_C = range;  // 앵커 C와의 거리 저장
                double x, y;
                calculatePosition(x, y);  // 태그의 위치 계산
                Serial.print("Found position - x: ");
                Serial.print(x); 
                Serial.print(" y: ");
                Serial.println(y);  // 계산된 위치 출력
                received_B = false;  // 상태 초기화
            } else {
                received_B = false;
            }
        }
    }
}
