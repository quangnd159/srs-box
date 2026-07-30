cmake_minimum_required(VERSION 3.16)
project(srs_sim CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# LVGL picks up lv_conf.h from this directory.
set(LV_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h CACHE STRING "" FORCE)
add_subdirectory(${ROOT}/third_party/lvgl ${CMAKE_BINARY_DIR}/lvgl)

add_executable(srs_sim
    main.cpp
    ${ROOT}/firmware/components/ui/review_ui.cpp
    ${ROOT}/firmware/components/ui/home_ui.cpp
    ${ROOT}/firmware/main/fonts/font_cjk_16.c
    ${ROOT}/firmware/main/fonts/font_cjk_20.c
    ${ROOT}/firmware/main/fonts/font_cjk_28.c
    ${ROOT}/firmware/main/fonts/font_cjk_48.c
)
target_include_directories(srs_sim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ROOT}/firmware/components/ui/include
    ${ROOT}/firmware/components/deck/include
    ${ROOT}/firmware/components/fsrs/include
    ${ROOT}/firmware/components/session/include
    ${ROOT}/firmware/components/persist/include
    ${ROOT}/firmware/components/power/include
)
target_link_libraries(srs_sim PRIVATE lvgl)
