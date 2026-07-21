cmake_minimum_required(VERSION 3.24)
project(BigHeadVPNNative LANGUAGES C CXX RC)

set(LSQPACK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.build-tools/ls-qpack")
add_library(lsqpack STATIC
    "${LSQPACK_DIR}/lsqpack.c"
    "${LSQPACK_DIR}/deps/xxhash/xxhash.c"
)
target_include_directories(lsqpack PUBLIC "${LSQPACK_DIR}" "${LSQPACK_DIR}/wincompat")
target_include_directories(lsqpack PRIVATE "${LSQPACK_DIR}/deps/xxhash")
target_compile_definitions(lsqpack PRIVATE LS_QPACK_USE_LARGE_TABLES=0)
target_compile_options(lsqpack PRIVATE -Os -w -ffunction-sections -fdata-sections)

add_executable(BigHeadVPN WIN32
    src/main.cpp
    src/model.cpp
    src/subscription.cpp
    src/quic_runtime.cpp
    src/hysteria_client.cpp
    src/socks_server.cpp
    resources/app.rc
)

target_include_directories(BigHeadVPN SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../.build-tools/msquic/include"
)

target_compile_features(BigHeadVPN PRIVATE cxx_std_20)
target_compile_definitions(BigHeadVPN PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000000
)
target_compile_options(BigHeadVPN PRIVATE
    -Os -ffunction-sections -fdata-sections -fno-rtti
    -Wall -Wextra -Wpedantic
)
target_link_options(BigHeadVPN PRIVATE
    -municode
    -static -static-libgcc -static-libstdc++
    -Wl,--gc-sections -Wl,--strip-all
)
target_link_libraries(BigHeadVPN PRIVATE
    lsqpack bcrypt comctl32 crypt32 dwmapi shlwapi winhttp ws2_32
)

add_executable(BigHeadVPNProbe
    tests/hysteria_probe.cpp
    src/hysteria_client.cpp
    src/socks_server.cpp
)
target_compile_features(BigHeadVPNProbe PRIVATE cxx_std_20)
target_compile_definitions(BigHeadVPNProbe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 BIG_HEAD_VPN_TESTING)
target_compile_options(BigHeadVPNProbe PRIVATE -Os -Wall -Wextra -Wpedantic)
target_include_directories(BigHeadVPNProbe SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../.build-tools/msquic/include"
    "${LSQPACK_DIR}"
)
target_include_directories(BigHeadVPNProbe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_options(BigHeadVPNProbe PRIVATE -municode -static -static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,--strip-all)
target_link_libraries(BigHeadVPNProbe PRIVATE lsqpack bcrypt ws2_32)
