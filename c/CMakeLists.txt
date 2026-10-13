# Native build — the library and everything that can be built from it WITHOUT ESP-IDF.
#
# Three things come out of here: the unit tests (`ctest`), `lagun-node` — a node over
# SocketCAN, the same code that goes onto a board — and `liblagunbus_sim`, a SHARED copy
# of the library with the catalogue of simulated devices (`sim/`), which Python loads
# through `ctypes`: the conformance tests (python/tests/conformance/) and the simulator
# `lagun-sim` (zadania/zrobione/FEAT-02.md).
#
# The board build has nothing to do with this file. `lagunbus/CMakeLists.txt` is the
# ESP-IDF component, and a firmware project picks it up through EXTRA_COMPONENT_DIRS.
# Same .c files, a different build.
cmake_minimum_required(VERSION 3.16)
project(lagunbus_native C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror -Wconversion)

set(LAGUNBUS_SOURCES
    lagunbus/src/frame.c
    lagunbus/src/values.c
    lagunbus/src/endpoint.c
    lagunbus/src/node.c
    lagunbus/src/tools/debounce.c
    lagunbus/src/tools/led.c
    lagunbus/src/tools/service.c
    lagunbus/src/types/display.c
    lagunbus/src/types/key.c
    lagunbus/src/types/output.c
    lagunbus/src/types/sense.c
)

add_library(lagunbus STATIC ${LAGUNBUS_SOURCES})
target_include_directories(lagunbus PUBLIC lagunbus/include)

# The shared copy is the library plus `sim/` — the device catalogue and a flat API over
# the node for `ctypes` (`sim/sim.h` says why not the structs directly). Built from the
# SOURCES rather than by wrapping the static library: a static archive is not
# position-independent by default, and turning that on for the test binaries too would
# change what `ctest` checks for no benefit.
add_library(lagunbus_sim SHARED ${LAGUNBUS_SOURCES} sim/sim.c sim/devices.c)
target_include_directories(lagunbus_sim PUBLIC lagunbus/include sim)

# The POSIX port — the reference port of the library. It builds EVERYWHERE; on a
# machine without SocketCAN what is left of it is a polite refusal instead of a socket.
add_library(lagunbus_posix
    ports/posix/transport.c
    ports/posix/platform.c
)
target_include_directories(lagunbus_posix PUBLIC ports/posix/include)
target_link_libraries(lagunbus_posix PUBLIC lagunbus)

enable_testing()

foreach(name frame values interfaces node key output led service)
    add_executable(test_${name} tests/test_${name}.c)
    target_link_libraries(test_${name} lagunbus_posix)
    target_include_directories(test_${name} PRIVATE tests)
    add_test(NAME ${name} COMMAND test_${name})
endforeach()

# `lagun-node` — the thing that can actually be run. On macOS it builds and politely
# refuses; on a Raspberry Pi it opens the bus.
add_executable(lagun-node lagun-node/lagun_node.c)
target_link_libraries(lagun-node lagunbus_posix)

# The registry has to agree with the specification — same as on the hub side.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(
        NAME registry
        COMMAND ${Python3_EXECUTABLE} tools/generate_registry.py --check
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
    )
endif()
