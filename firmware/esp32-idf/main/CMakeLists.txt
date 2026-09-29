idf_component_register(SRCS "bridge.cpp" "media_codec.cpp" INCLUDE_DIRS "." "../../../common"
    REQUIRES bt nvs_flash esp_driver_i2s esp_driver_spi esp_driver_uart esp_driver_gpio esp_timer esp_audio_codec)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-deprecated-declarations)
target_include_directories(${COMPONENT_LIB} PRIVATE
    "$ENV{IDF_PATH}/components/bt/host/bluedroid/external/sbc/decoder/include")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=btc_a2dp_sink_enque_buf")
