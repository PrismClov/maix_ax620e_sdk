############### includes ###############
list(APPEND ADD_INCLUDE "include" "include/jpeg" "." "app" "app/common" "app/camera"
                        "app/pages" "app/settings" "wifi" "lvgl")

############### sources ###############
file(GLOB_RECURSE LVGL_SRCS "lvgl/src/*.c")
list(FILTER LVGL_SRCS EXCLUDE REGEX "lvgl/src/drivers/")
file(GLOB APP_SRCS "app/*.c" "app/*/*.c" "app/*/*/*.c")
file(GLOB WIFI_SRCS "wifi/*.c")
file(GLOB_RECURSE THORVG_SRCS "lvgl/src/libs/thorvg/*.cpp")

set(EXCLUDE_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/app/camera/camera_v4l2.c"
    "${CMAKE_CURRENT_LIST_DIR}/app/camera/rga_accel.c"
    "${CMAKE_CURRENT_LIST_DIR}/app/camera/k230_accel.c"
    "${CMAKE_CURRENT_LIST_DIR}/app/camera/recorder_rkmpp.c")

list(REMOVE_ITEM APP_SRCS ${EXCLUDE_SRCS})

list(APPEND ADD_SRCS ${LVGL_SRCS} ${THORVG_SRCS} ${APP_SRCS}
     "src/main.cpp" "src/port.cpp" "src/cam_maix.cpp" "src/recorder_rkmpp_stub.c")

list(APPEND ADD_DEFINITIONS_PRIVATE -DLV_CONF_INCLUDE_SIMPLE -DAPP_WIFI_DISABLED)

############### deps ###############
list(APPEND ADD_REQUIREMENTS basic vision)

############### libjpeg (from target rootfs) ###############
list(APPEND ADD_LINK_SEARCH_PATH "${CMAKE_CURRENT_LIST_DIR}/lib")
list(APPEND ADD_STATIC_LIB "${CMAKE_CURRENT_LIST_DIR}/lib/libjpeg.a")

register_component()
