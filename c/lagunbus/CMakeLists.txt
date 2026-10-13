# The library as an ESP-IDF component.
#
# The very same .c files are built natively by `c/CMakeLists.txt`. There are no
# conditions on the platform or on ESP-IDF here, because this directory holds NOTHING
# that would need them — hardware begins in the ports (`c/ports/posix` in this
# repository, `ports/esp32c3` in `devices`).
idf_component_register(
    SRCS "src/frame.c" "src/values.c" "src/endpoint.c" "src/node.c"
         "src/tools/debounce.c" "src/tools/led.c" "src/tools/service.c"
         "src/types/display.c" "src/types/key.c" "src/types/output.c" "src/types/sense.c"
    INCLUDE_DIRS "include"
)
