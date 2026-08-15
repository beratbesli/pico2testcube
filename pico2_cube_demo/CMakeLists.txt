cmake_minimum_required(VERSION 3.13)

# Target Raspberry Pi Pico 2 (RP2350) by default
set(PICO_BOARD pico2 CACHE STRING "Board type")

include(pico_sdk_import.cmake)

project(pico2_cube_demo C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Raspberry Pi Pico SDK
pico_sdk_init()

add_executable(pico2_cube_demo
    main.c
)

# Pull in core SDK libraries and hardware drivers
target_link_libraries(pico2_cube_demo
    pico_stdlib
    hardware_clocks
    hardware_gpio
)

# Enable USB CDC stdio, disable hardware UART
pico_enable_stdio_usb(pico2_cube_demo 1)
pico_enable_stdio_uart(pico2_cube_demo 0)

# Generate UF2, BIN, HEX and map files
pico_add_extra_outputs(pico2_cube_demo)
