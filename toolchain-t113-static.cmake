set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(CMAKE_C_COMPILER   arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)

# Статика: чтобы не зависеть от glibc на плате (у вас 2.25)
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -O2 -static")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -static")
