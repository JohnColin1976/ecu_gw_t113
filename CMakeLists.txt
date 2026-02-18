cmake_minimum_required(VERSION 3.16)

project(ecu_qw_t113 C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR}/include)

add_library(ecu_proto
  src/ecu/ecu_crc16.c
  src/ecu/ecu_proto.c
  src/ecu/ecu_slip.c
)

add_executable(test_slip_frame
  tests/test_slip_frame.c
)

target_link_libraries(test_slip_frame ecu_proto)

