#pragma once
#include <stdint.h>

#define ECU_MAGIC              0xEC10u
#define ECU_VERSION            1u

#define ECU_MAX_PAYLOAD        1024u
#define ECU_HEADER_SIZE        16u
#define ECU_CRC_SIZE           2u
#define ECU_MAX_FRAME_SIZE     (ECU_HEADER_SIZE + ECU_MAX_PAYLOAD + ECU_CRC_SIZE)

// Node IDs
#define ECU_NODE_BROADCAST     0u
#define ECU_NODE_PC            0u      // используем как "PC client" на Ethernet-сегменте
#define ECU_NODE_GW            255u    // RP-T113 gateway

// Для проекта: статическая привязка по умолчанию
#define ECU_NODE1              1u
#define ECU_NODE2              2u
#define ECU_NODE3              3u
