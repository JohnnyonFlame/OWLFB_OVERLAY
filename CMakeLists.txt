cmake_minimum_required(VERSION 3.13.4)
project(owlfb_overlay)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

file(GLOB SOURCES *.c)

add_executable(owlfb_overlay ${SOURCES})
target_link_libraries(owlfb_overlay PRIVATE pthread ${ASTC_ENC_TARGET})