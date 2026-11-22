cmake_minimum_required(VERSION 3.20)
project(wwhd_relay CXX)

set(CMAKE_CXX_STANDARD 17)

# ENet (same version as the game runtime)
include(FetchContent)
FetchContent_Declare(enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG        v1.3.17
)
FetchContent_MakeAvailable(enet)

add_executable(wwhd_relay relay_server.cpp)
# ENet 1.3.x doesn't export include dirs via its CMake target; add them manually.
target_include_directories(wwhd_relay PRIVATE ${enet_SOURCE_DIR}/include)
target_link_libraries(wwhd_relay PRIVATE enet)

if(WIN32)
    target_link_libraries(wwhd_relay PRIVATE ws2_32 winmm)
endif()

# Build:  cmake -S relay -B build/relay -DCMAKE_BUILD_TYPE=Release
#         cmake --build build/relay
# Run:    ./build/relay/wwhd_relay 7778
