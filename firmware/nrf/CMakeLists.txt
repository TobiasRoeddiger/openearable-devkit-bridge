cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(openearable_adapter)
target_include_directories(app PRIVATE ../../common src)
target_sources(app PRIVATE src/main.c src/link.c src/pcm.c src/earables.c)
# Build-local hook: do not block Bluetooth RX while awaiting the earable's ATT reply.
set(ATT_SOURCE ${ZEPHYR_BASE}/subsys/bluetooth/host/att.c)
set(ATT_PROXY_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/adapter_att.c)
add_custom_command(OUTPUT ${ATT_PROXY_SOURCE}
    COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/att_hook.py
        ${ATT_SOURCE} ${ATT_PROXY_SOURCE}
    DEPENDS ${ATT_SOURCE} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/att_hook.py
    VERBATIM)
add_custom_target(adapter_att_hook DEPENDS ${ATT_PROXY_SOURCE})
add_dependencies(subsys__bluetooth__host adapter_att_hook)
get_target_property(HOST_SOURCES subsys__bluetooth__host SOURCES)
list(FILTER HOST_SOURCES EXCLUDE REGEX "(^|/)att\\.c$")
set_property(TARGET subsys__bluetooth__host PROPERTY SOURCES ${HOST_SOURCES} ${ATT_PROXY_SOURCE})
target_include_directories(subsys__bluetooth__host PRIVATE ${ZEPHYR_BASE}/subsys/bluetooth/host)
target_sources(app PRIVATE src/proxy.c)
