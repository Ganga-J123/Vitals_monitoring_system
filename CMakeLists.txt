cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(Vitals_monitoring_system)

# Add src/ as an include directory so headers like ble.h are found
target_include_directories(app PRIVATE src)

# Add all your C source files
target_sources(app PRIVATE
    src/main.c
    src/ble.c
    src/adpd144ri.c
    src/rtc.c
    src/temp.c
    src/lis3dh.c
    src/ws2812.c
    src/button.c
)
