##
# @file CMakeLists.txt
# @brief T5-Auto-meter — circular AMOLED OBD-II gauge cluster
#/

set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})

get_filename_component(APP_NAME ${APP_PATH} NAME)

# Aggregate sources from src/ and known subdirectories
aux_source_directory(${APP_PATH}/src APP_SRC)
foreach(dir board sensor obd app ui ui/fonts)
    aux_source_directory(${APP_PATH}/src/${dir} SUB_SRC)
    list(APPEND APP_SRC ${SUB_SRC})
endforeach()

set(APP_INC
    ${APP_PATH}/include
    ${APP_PATH}/src
    ${APP_PATH}/src/board
    ${APP_PATH}/src/sensor
    ${APP_PATH}/src/obd
    ${APP_PATH}/src/app
    ${APP_PATH}/src/ui
    ${APP_PATH}/src/ui/fonts
)

set(APP_OPTIONS "-W")
list(APPEND APP_OPTIONS "-Wall" "-Wno-unused-function" "-Wno-unused-variable" "-DLV_LVGL_H_INCLUDE_SIMPLE")

########################################
# Target Configure
########################################
add_library(${EXAMPLE_LIB})
message(STATUS "EXAMPLE_LIB:${APP_PATH}")

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRC}
    )

target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_INC}
    )

target_compile_options(${EXAMPLE_LIB}
    PRIVATE
        ${APP_OPTIONS}
    )
