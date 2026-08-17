file(GLOB GNUBOY_SRCS "gnuboy/*.c")

idf_component_register(
    SRCS 
        "main.cpp"
        "SdUsbManager.cpp"
        ${GNUBOY_SRCS}
    INCLUDE_DIRS 
        "."
        "gnuboy"
    REQUIRES 
        "app_update"
        "esp_partition"
        "bsp_board_extra"
        "ui_common"
        "esp_lcd"
        "lvgl"
        "fatfs"
        "nvs_flash"
        "esp_timer"
)

set_source_files_properties(${GNUBOY_SRCS} PROPERTIES COMPILE_FLAGS "-Wno-error -Wno-implicit-function-declaration -Wno-int-conversion")