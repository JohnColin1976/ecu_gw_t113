cmake_minimum_required(VERSION 3.16)

project(ecu_gw_t113 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR}/include)

add_library(ecu_proto
  src/ecu/ecu_crc16.c
  src/ecu/ecu_proto.c
  src/ecu/ecu_slip.c
)

add_executable(ecu_gw
  src/main.c
  src/gw/gw_app.c
  src/gw/gw_cmd_ui.c
  src/gw/gw_net.c
  src/gw/gw_router.c
  src/gw/gw_uart.c
)

target_link_libraries(ecu_gw ecu_proto)
