# Patch Plan: интерактивный `-cmd_ui PORT` для отправки COMMAND и просмотра ответов UART

## 1. Цель изменения
Добавить новый режим запуска приложения:
- `ecu_gw -cmd_ui PORT`, где `PORT` строго один из: `ttyS1`, `ttyS4`, `ttyS5`.

В этом режиме программа:
- очищает консоль;
- делит экран на 2 области:
  - верхняя/левая: ввод команды пользователем;
  - нижняя/правая: поток принятых ответов с выбранного UART;
- по `Enter` валидирует введённую команду по спецификации `src/docs/protocol_v1_0.md`;
- при валидной команде формирует ECU frame типа `COMMAND` и отправляет в выбранный UART через SLIP;
- принимает входящие ECU-кадры с того же UART и отображает их в области ответов.

## 2. Принятые ограничения и соответствие протоколу
Основано на `src/docs/protocol_v1_0.md`:
- Little-endian поля;
- заголовок `ecu_hdr_t` (16 байт), `version=1`, `magic=0xEC10`;
- отправляем только `msg_type=COMMAND (0x03)`;
- для `COMMAND` обязательно выставлять `ACK_REQUIRED` (`flags bit 0`);
- payload `COMMAND`: `command_id:u16`, `param_len:u16`, `param_data[]`;
- `payload_len <= 1024`, итоговый frame `header + payload + crc16-ccitt`;
- `crc` считается на `header + payload`.

Уточнение по адресации:
- `src = ECU_NODE_GW (255)`;
- `dst` выбирается по порту:
  - `ttyS1 -> ECU_NODE1`;
  - `ttyS4 -> ECU_NODE2`;
  - `ttyS5 -> ECU_NODE3`.

## 3. Формат пользовательской команды (MVP)
Чтобы ввести команды «в соответствии со спецификацией» и при этом иметь однозначный парсинг, вводим текстовый DSL для UI:

```text
cmd <command_id> [hex_bytes]
```

Примеры:
- `cmd 7`                      -> `PING`, `param_len=0`
- `cmd 2 00 10`                -> `SET_TARGET_RPM`, 2 байта параметров
- `cmd 8`                      -> `ENTER_BOOT`, строго без payload

Правила валидации:
- префикс `cmd` обязателен;
- `command_id` в диапазоне `1..8` (v1 из документа);
- байты параметров только в `HEX` (`00..FF`), через пробел;
- `param_len == числу hex-байт`;
- для `ENTER_BOOT (8)` разрешён только `param_len=0`;
- `sizeof(command_hdr)+param_len <= ECU_MAX_PAYLOAD`.

Примечание: это не меняет wire-format, только определяет формат CLI-ввода.

## 4. Изменения по файлам

### 4.1 `src/main.c`
- Добавить парсинг новой опции `-cmd_ui PORT`.
- Валидация, что `PORT` присутствует и указан не вместе с `-send_test`.
- Обновить usage:
  - `-show`
  - `-prev_show`
  - `-send_test PORT`
  - `-cmd_ui PORT`
- Передать новый аргумент в `gw_app_run(...)` (расширение сигнатуры).

### 4.2 `include/gw/gw_app.h`
- Обновить сигнатуру:
  - было: `int gw_app_run(int show_packets, int preview_raw, const char* send_test_ports);`
  - станет: `int gw_app_run(int show_packets, int preview_raw, const char* send_test_ports, const char* cmd_ui_port);`

### 4.3 `src/gw/gw_app.c`
Добавить новый ранний режим `gw_app_run_cmd_ui(const char* port_name)` и вызывать его из `gw_app_run(...)`, если задан `cmd_ui_port`.

Внутри `gw_app_run_cmd_ui`:
- Открыть только выбранный UART (`/dev/ttyS1|4|5`) через существующий `gw_uart_open`.
- Настроить терминал в raw/cbreak-режим для неблокирующего ввода с клавиатуры (через `termios` + восстановление при выходе).
- Очистить экран ANSI-последовательностью (`\x1b[2J\x1b[H`).
- Отрисовать layout из 2 панелей (ANSI cursor positioning):
  - панель Input (1-2 строки + статус валидации);
  - панель RX log (скроллинг текстовых строк сверху вниз).
- Основной event-loop на `epoll`:
  - `stdin` (readline-подобный буфер до `Enter`);
  - `uart fd` на `EPOLLIN` и `EPOLLOUT` при наличии TX очереди.
- На `Enter`:
  - распарсить текст в структуру `command_id + param_bytes`;
  - выполнить валидацию правил из раздела 3;
  - при ошибке вывести строку ошибки в input/status зоне;
  - при успехе собрать ECU frame (`msg_type=COMMAND`, `ACK_REQUIRED`, `seq++`, `src/dst`, crc);
  - отправить через `gw_uart_send_slip`, включить `EPOLLOUT` через `ep_mod`.
- При чтении UART:
  - `gw_uart_handle_read` -> `gw_uart_try_get_slip_frame`;
  - валидировать ECU frame существующей `validate_ecu_bytes`;
  - выводить в RX-панель краткий разбор:
    - timestamp локальный;
    - `msg_type`, `seq`, `flags`, `payload_len`;
    - для `ACK` дополнительно `ack_seq/status_code`;
    - fallback: hex payload.
- Горячие клавиши:
  - `Ctrl+C`/`q` для выхода.
- Гарантированное восстановление терминала и закрытие UART при любом выходе.

### 4.4 (опционально, но рекомендовано) новый модуль UI
Чтобы не перегружать `gw_app.c`, вынести в:
- `include/gw/gw_cmd_ui.h`
- `src/gw/gw_cmd_ui.c`

Содержимое:
- парсер текстовой команды;
- сборка `COMMAND` ECU-frame;
- функции рендера TUI (ANSI);
- run-loop для режима `-cmd_ui`.

Тогда `gw_app.c` только роутит в `gw_cmd_ui_run(port)`.

### 4.5 `CMakeLists.txt`
Если выносим в отдельный модуль, добавить `src/gw/gw_cmd_ui.c` в `add_executable(ecu_gw ...)`.

### 4.6 `README.md`
- Добавить раздел по `-cmd_ui`:
  - синтаксис запуска;
  - примеры ввода команд;
  - как выйти;
  - что отображается в RX-области.

## 5. Детализация wire-format при отправке COMMAND
Для каждой валидной введённой команды формируется:

- Header:
  - `magic = ECU_MAGIC`
  - `version = ECU_VERSION`
  - `msg_type = ECU_MSG_COMMAND`
  - `src = ECU_NODE_GW`
  - `dst = node_by_port`
  - `seq = next_seq`
  - `flags = ECU_F_ACK_REQUIRED`
  - `payload_len = 4 + param_len`
  - `reserved1 = 0`, `reserved2 = 0`

- Payload:
  - `uint16_t command_id`
  - `uint16_t param_len`
  - `uint8_t param_data[param_len]`

- Trailer:
  - `uint16_t crc = ecu_frame_calc_crc2(&hdr, payload)`

Отправка:
- Собранные bytes кадра передаются в `gw_uart_send_slip(...)`.

## 6. Этапы внедрения
1. Расширить CLI (`main.c`, `gw_app.h`) и сделать диспатч режима `-cmd_ui`.
2. Реализовать минимальный `cmd_ui` loop без «красивого» UI (очистка экрана + 2 логические области).
3. Добавить парсер/валидатор текстовых команд и сборщик ECU `COMMAND`.
4. Добавить приём/декодирование ответов UART и форматированный вывод.
5. Укрепить UX (редактирование строки: backspace, ограничение длины, подсказка формата).
6. Обновить README и usage.

## 7. Проверки и тест-план

### 7.1 Ручные проверки
- Запуск: `ecu_gw -cmd_ui ttyS1` (аналогично `ttyS4`, `ttyS5`).
- Экран очищается и видно 2 области.
- Ввод `cmd 7` + `Enter` -> кадр уходит в UART (нет ошибок очереди).
- Ввод невалидной строки (`cmd x`, `cmd 8 01`) -> корректная ошибка в input/status.
- При входящих кадрах отображается запись в RX-панели.
- Выход по `q` и `Ctrl+C` восстанавливает терминал.

### 7.2 Протокольные проверки
- Убедиться, что `msg_type=COMMAND`, `flags` содержит `ACK_REQUIRED`.
- Проверить `dst` соответствует выбранному `ttyS*`.
- Проверить CRC валиден на стороне приёмника.

### 7.3 Регрессия
- Существующие режимы `-show`, `-prev_show`, `-send_test` работают без изменений.

## 8. Риски и меры
- Риск: конфликт terminal raw mode с аварийным завершением.
  - Мера: единая функция cleanup + `atexit`/обработка сигналов.
- Риск: потеря части RX при упрощённой логике `gw_uart_try_get_slip_frame`.
  - Мера: в рамках patch не менять поведение; при необходимости отдельный patch для точного consumed-bytes в SLIP.
- Риск: неоднозначный пользовательский формат команды.
  - Мера: фиксировать DSL `cmd <id> [hex...]` и печатать help в input-зоне.

## 9. Критерий готовности
Изменение считается завершённым, когда:
- `-cmd_ui PORT` работает для всех трёх портов;
- валидные команды отправляются как корректные ECU `COMMAND` кадры;
- невалидные команды не отправляются и дают понятную ошибку;
- ответы с UART отображаются в отдельной области;
- старые режимы не сломаны.
