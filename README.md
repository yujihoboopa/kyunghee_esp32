# ESP32 Lua 5.1.5 GC 실험 펌웨어

ESP32-WROOM-32용 ESP-IDF 프로젝트입니다. `components/lua51`은 저장소에 포함된 Lua 5.1.5 소스를 빌드하며 `lgc.c`에 단계별 타이머를 삽입합니다. Lua allocator는 내부 8-bit SRAM heap을 사용합니다.

## 빌드 및 실행

이 프로젝트는 ESP-IDF 6.x의 NimBLE 포트를 사용합니다. 실험 계획서의 ESP-IDF 6.1에서 빌드하려면 IDF_PATH를 6.1 설치 경로로 설정하세요. 개발 환경에 6.2가 설치된 경우에도 다음 빌드로 API 호환 여부를 확인할 수 있습니다.

```sh
. /path/to/esp-idf/export.sh
idf.py set-target esp32
idf.py build
idf.py -p PORT flash monitor
```

광고 이름은 `LuaGC-ESP32`이며, 연결 후 128-bit 실험 서비스의 Notify characteristic을 구독하면 센서 시퀀스 알림을 받습니다.

## 실행되는 실험 흐름

- 부팅 때 단순/중첩 테이블, closure/upvalue, weak-value table, 일반 full userdata, `__gc` finalizer userdata, C API 스택 root 조건을 각각 30회 반복합니다. 각 조건의 생성량, allocator 추적 회수량, forced-GC 시간과 생존/weak-value 수를 요약합니다.
- Lua 초기화와 객체군 실험은 16 KB 스택의 전용 setup task에서 실행해 ESP-IDF의 기본 `main` task 스택을 소진하지 않도록 했습니다.
- 주기 부하는 100 ms마다 실행되며 12개 프로파일을 각 100회 호출(명목 10초)씩 순환합니다. 네 Core 배치의 baseline, 같은 GC 부하의 네 Core 배치, 높은 부하에서 기본 설정/pause=100/stepmul=400/강제 전체 GC 비교를 포함합니다. 한 바퀴는 명목상 약 120초입니다.
- GC 내부 phase는 RAM 누적기에 count/total/max를 기록하고 2초마다 단계별 요약만 출력합니다. per-step UART 출력은 하지 않습니다.
- Lua 태스크 지연과 Lua 함수 실행시간은 20개 표본 단위 min/mean/P95/max로 요약합니다. 별도 10 ms 측정 태스크는 100개 표본마다 요약합니다. Lua와 측정 태스크의 코어 배치는 프로파일마다 변경하며 BLE 송신 태스크는 Core 0에 고정됩니다.
- NimBLE Central에서 Notify characteristic을 구독하면 100 ms마다 `sensor,<seq>,<profile>` 데이터를 받습니다. 연결 및 구독 상태와 송신 성공/실패 요약을 UART에 표시합니다. notify API 성공은 실제 수신 완료를 뜻하지 않습니다.

## 로그 수집 및 분석

새 바이너리를 플래시하고 UART monitor를 켭니다. 다른 호스트 터미널에서 `bleak` 기반 Central 캡처를 시작해 자동 연결/Notify 구독을 수행하고 120초 이상 유지하세요. 스마트폰을 Central로 쓰면 수신 로그를 별도로 내보내야 합니다.

```sh
python3 -m pip install bleak
python3 tools/ble_central_capture.py \
  --output results/ble-central.csv \
  --summary results/ble-central-summary.csv
```

Central 캡처를 Ctrl-C로 정상 종료한 뒤 UART 로그를 `results/uart-capture.log`에 저장하고 다음 파서를 실행합니다. Central 프로그램은 GATT read로 ESP32 `esp_timer`와 호스트 monotonic clock의 오프셋을 여러 번 추정해 지연값과 오차 추정치를 함께 기록합니다.

```sh
python3 tools/parse_uart_capture.py
```

원본 `uart-capture.log`는 이전 펌웨어의 파일럿 기록이며 새 요약 출력 수정 전 데이터입니다. 새 펌웨어에서 다시 수집해 덮어쓰지 않도록 이전 로그를 별도 파일명으로 보존하세요.

Central 수신 시각과 패킷 손실은 Central 측 로그를 시퀀스 번호와 연결해야 분석할 수 있습니다. 객체군 30회 반복은 한 부팅에서 수행하므로 최종 결론 전 여러 번 재부팅해 재현성을 확인하세요.
