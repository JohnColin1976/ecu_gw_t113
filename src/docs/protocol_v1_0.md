### 1. Общие принципы
- бинарный протокол
- Little-Endian
> Порядок хранения многобайтовых чисел в памяти и при передаче
   младший (LSB) байт идет первым
   старший байт (MSB) идет последним
   Возьмем число 0х1234
   В памяти (Little-Endian):
   адрес +0 хранит значение 0х34
   адрес +1 хранит значение 0х12
   Почему это важно:
   Arduino Due (ARM Cortex-M3) -> Little-Endian
   RP-T113 (ARM) -> Little-Endian
   x86 ПК -> Little-Endian
   Это удобно:
   можно memcpy структуры
   можно читать бинарные данные напрямую
   не нужно бфйт-свапать
- Версионность обязательна
- CRC для защиты
- Нумерация пакетов (SEQ)
- Поддержка ACK / NACK
- Адресация узлов (Node 1...3)
- Расширяемость
---
### 2. Адресация
- 0 - Broadcast
- 1 - Arduino Due #1 (UART1)
- 2 - Arduino Due #2 (UART 4)
- 3 - Arduino Due #3 (UART 5)
- 255 - RT-T113 (gateway)
---
### 3. Транспортные уровни
#### 3.1 UART (T113 <-> Due)
Используется SLIP framing
- бинарный пакет
- CRC16-CCIT
#### 3.2 Ethernet (PC <-> T113)
TCP соединение.
Формат
```cpp
uint32_t frame_lenght
[ECU frame bytes]
```
(без SLIP, т.к. TCP гарантирует границы через lenght prefix)

---
### 4. Формат кадра (ECU Frame)
#### 4.1 Заголовок (фиксированный 16 байт)

| Поле        | Размер | Описание            |
| ----------- | ------ | ------------------- |
| magic       | u16    | 0xEC10              |
| version     | u8     | 1                   |
| msg_tytpe   | u8     | тип сообщения       |
| src         | u8     | NodeID отправвителя |
| dst         | u8     | NodeID получателя   |
| seq         | u16    | номер пакета        |
| flags       | u16    | биты управления     |
| payload_len | u16    | длина payload       |
| reserved    | u16    | =0 (для будущего)   |
Итого: 16 байт.

#### 4.2 Payload
Переменной длины (0.1024 байт)

#### 4.3 CRC
CRC16-CCIT (poly 0x1024, int 0xFFFF)
Считается
```cpp
header + payload
```
В конце кадра добавляется:
```cpp
uint16_t crc
```

---
### 5. Flags (u16)

| Бит  | Назначение   |
| ---- | ------------ |
| 0    | ACK_REQUIRED |
| 1    | IS_ACK       |
| 2    | IS_NASK      |
| 3    | ERROR        |
| 4    | URGENT       |
| 5-15 | reserved     |

---
### 6. Типы сообщений (msg_type)

| Значение | Имя       |
| -------- | --------- |
| 0x01     | HELLO     |
| 0x02     | TELEMETRY |
| 0x03     | COMMAND   |
| 0x04     | ACK       |
| 0x05     | TIME_SYNC |
| 0x06     | EVENT     |
| 0x07     | CONFIG    |
| 0x08     | HEARTBEAT |

---
### 7. Описание типов сообщений

#### 7.1 HELLO
Отправляется Due при старте
Payload:
```cpp
uint8_t node_id
uint32_t fw_version
uint32_t build_time
uint32_t capabilities_mask
```
Ответ:
ACK

#### 7.2 TELEMETRY
Передается периодически (например 50 Гц)
Payload v1:
```cpp
uint32_t uptime_ms
uint16_t status_flags
uint16_t error_code
float voltage
float current
float temperature
float rpm
```
Без ACK (flags=0)

#### 7.3 COMMAND
Отправляется от PC -> T113 -> Due
Payload:
```cpp
uint16_t command_id
uint16_t param_len
uint8_t param_data[]
```
Флаг:
ACK_REQUIRED = 1

Command ID (v1)

| ID  | Назначение        |
| --- | ----------------- |
| 1   | SET_MODE          |
| 2   | SET_TARGET_RPM    |
| 3   | SET_LIMIT_CURRENT |
| 4   | ARM               |
| 5   | DISARM            |
| 6   | RESET_FAULT       |
| 7   | PING              |
| 8   | ENTER_BOOT        |

`ENTER_BOOT`:
- Payload отсутствует (`param_len = 0`)
- При получении команда инициирует вход в загрузчик:
  - запись `MAGIC_UPDATE` в `GPBR[0]`
  - `NVIC_SystemReset()`

#### 7.4 ACK
Payload:
```cpp
uint16_t ack_seq
uint16_t status_code
```
status_code:
0 = OK
1 = UNKNOWN_COMMAND
2 = INVALID_PARAM
3 = INTERNAL_ERROR

#### 7.5 TIME_SYNC
Payload:
```cpp
uint64_t unix_time_ms
```

#### 7.6 EVENT
Payload:
```cpp
uint16_t event_code
uint16_t data_len
uint8_t data[]
```

#### 7.7 HEARTBEAT
Пустой payload.
Интервал: 1 сек.
Если нет heartbeat > 3 сек -> узел offline.

---
### 8. Поведение протокола

#### 8.1 Запуск системы
1. Due -> HELLO
2. T113 -> ACK
3. T113 -> TIME_SYNC
4. Due -> TELEMETRY начинает поток

#### 8.2 Команда
1. PC -> T113 (COMMAND, dst=node)
2. T113 -> соответствующий UART
3. Due -> ACK
4. T113 -> PC ACK
Retry:
- если нет ACK 200mc -> повторить (max 3 раза)

#### 8.3 Обнаружение потери связи
Если:
- нет TELEMETRY > 2 сек
- нет HEARTBEAT > 3 сек
-> узел offline

---
### 9. Ограничения v1.0
- Максимальный payload: 1024 байта
- Максимальная скорость UART: 230400 (рекомендовано 115200)
- Поддерживается до 255 узлов (но сейчас 3)

---
### 10. Расширяемость
- version поле позволяет менять структуру
- reserved зарезервировано
- новые msg_type добавляются без ломания старых
- payload версии можно менять через version внутри payload
