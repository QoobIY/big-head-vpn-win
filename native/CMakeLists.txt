cmake_minimum_required(VERSION 3.24)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" BIG_HEAD_VERSION)
string(STRIP "${BIG_HEAD_VERSION}" BIG_HEAD_VERSION)
if(NOT BIG_HEAD_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VERSION must contain major.minor.patch, for example 0.1.0")
endif()
project(BigHeadVPNNative VERSION "${BIG_HEAD_VERSION}" LANGUAGES C CXX RC)
foreach(component PROJECT_VERSION_MAJOR PROJECT_VERSION_MINOR PROJECT_VERSION_PATCH)
    if(${component} GREATER 65535)
        message(FATAL_ERROR "Windows version components must be <= 65535")
    endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
foreach(resource version.h version-resource.rc app.manifest startup.manifest app.rc startup.rc)
    configure_file("resources/${resource}.in" "generated/${resource}" @ONLY)
endforeach()

set(LSQPACK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.build-tools/ls-qpack")
set(NGHTTP2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.build-tools/nghttp2")
set(MBEDTLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.build-tools/mbedtls")
add_library(lsqpack STATIC
    "${LSQPACK_DIR}/lsqpack.c"
    "${LSQPACK_DIR}/deps/xxhash/xxhash.c"
)
target_include_directories(lsqpack PUBLIC "${LSQPACK_DIR}" "${LSQPACK_DIR}/wincompat")
target_include_directories(lsqpack PRIVATE "${LSQPACK_DIR}/deps/xxhash")
target_compile_definitions(lsqpack PRIVATE LS_QPACK_USE_LARGE_TABLES=0)
target_compile_options(lsqpack PRIVATE -Os -w -ffunction-sections -fdata-sections)

file(GLOB NGHTTP2_SOURCES "${NGHTTP2_DIR}/lib/*.c")
add_library(nghttp2_static STATIC ${NGHTTP2_SOURCES})
target_include_directories(nghttp2_static PUBLIC "${NGHTTP2_DIR}/lib/includes")
target_include_directories(nghttp2_static PRIVATE "${NGHTTP2_DIR}/lib")
target_compile_definitions(nghttp2_static PUBLIC NGHTTP2_STATICLIB)
target_compile_options(nghttp2_static PRIVATE -Os -w -ffunction-sections -fdata-sections)

set(PQCLEAN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/pqclean")
file(GLOB MLKEM_SOURCES CONFIGURE_DEPENDS "${PQCLEAN_DIR}/crypto_kem/ml-kem-768/clean/*.c")
add_library(mlkem768 STATIC ${MLKEM_SOURCES}
    "${PQCLEAN_DIR}/common/fips202.c" "${PQCLEAN_DIR}/common/randombytes.c")
target_include_directories(mlkem768 PUBLIC "${PQCLEAN_DIR}/crypto_kem/ml-kem-768/clean")
target_include_directories(mlkem768 PRIVATE "${PQCLEAN_DIR}/common")
target_compile_options(mlkem768 PRIVATE -Os -ffunction-sections -fdata-sections)

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
add_subdirectory("${MBEDTLS_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/mbedtls" EXCLUDE_FROM_ALL)
foreach(mbed_target mbedtls mbedx509 mbedcrypto)
    target_compile_options(${mbed_target} PRIVATE -Os -w -ffunction-sections -fdata-sections)
endforeach()

add_executable(BigHeadVPN WIN32
    src/main.cpp
    src/model.cpp
    src/subscription.cpp
    src/tunnel_client.cpp
    src/quic_runtime.cpp
    src/hysteria_client.cpp
    src/vless_client.cpp
    src/vless_grpc_client.cpp
    src/vless_vision_client.cpp
    src/reality_tls.cpp
    src/reality_hybrid.cpp
    src/schannel_tls.cpp
    src/socks_server.cpp
    src/process_filter.cpp
    src/autostart.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc"
)

# Recompile Windows resources when the shared application icon changes.
set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc" APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/big-head-vpn.ico"
    "${CMAKE_CURRENT_SOURCE_DIR}/resources/Manrope-Variable.ttf"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version-resource.rc"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/app.manifest")

target_include_directories(BigHeadVPN PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_include_directories(BigHeadVPN SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../.build-tools/msquic/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../.build-tools/windivert"
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
    mlkem768 lsqpack nghttp2_static mbedtls mbedx509 mbedcrypto advapi32 bcrypt comctl32 crypt32 dwmapi iphlpapi oleaut32 secur32 shell32 shlwapi taskschd uxtheme winhttp ws2_32
)

add_executable(BigHeadVPNProbe
    tests/hysteria_probe.cpp
    src/autostart.cpp
    src/tunnel_client.cpp
    src/hysteria_client.cpp
    src/vless_client.cpp
    src/vless_grpc_client.cpp
    src/vless_vision_client.cpp
    src/reality_tls.cpp
    src/reality_hybrid.cpp
    src/schannel_tls.cpp
    src/http2_connection.cpp
    src/socks_server.cpp
    src/process_filter.cpp
)
target_compile_features(BigHeadVPNProbe PRIVATE cxx_std_20)
target_compile_definitions(BigHeadVPNProbe PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 BIG_HEAD_VPN_TESTING)
target_compile_options(BigHeadVPNProbe PRIVATE -Os -Wall -Wextra -Wpedantic)
target_include_directories(BigHeadVPNProbe SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../.build-tools/msquic/include"
    "${LSQPACK_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../.build-tools/windivert"
)
target_include_directories(BigHeadVPNProbe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_options(BigHeadVPNProbe PRIVATE -municode -static -static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,--strip-all)
target_link_libraries(BigHeadVPNProbe PRIVATE mlkem768 lsqpack nghttp2_static mbedtls mbedx509 mbedcrypto advapi32 bcrypt iphlpapi oleaut32 secur32 shell32 taskschd winhttp ws2_32)

add_executable(BigHeadVPNHybridTest EXCLUDE_FROM_ALL
    tests/reality_hybrid_test.cpp src/reality_hybrid.cpp)
target_include_directories(BigHeadVPNHybridTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(BigHeadVPNHybridTest PRIVATE cxx_std_20)
target_compile_options(BigHeadVPNHybridTest PRIVATE -Wall -Wextra -Wpedantic)
target_link_options(BigHeadVPNHybridTest PRIVATE -static -static-libgcc -static-libstdc++)
target_link_libraries(BigHeadVPNHybridTest PRIVATE mlkem768 mbedcrypto bcrypt advapi32)

# Unelevated, windowless entry for Explorer's Startup list. The task launches
# the main requireAdministrator executable after startup is enabled by the user.
add_executable(BigHeadVPNStartup WIN32 src/startup_main.cpp src/autostart.cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/startup.rc")
target_compile_features(BigHeadVPNStartup PRIVATE cxx_std_20)
target_compile_definitions(BigHeadVPNStartup PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_compile_options(BigHeadVPNStartup PRIVATE -Os -Wall -Wextra -Wpedantic)
target_link_options(BigHeadVPNStartup PRIVATE -municode -static -static-libgcc -static-libstdc++ -Wl,--gc-sections -Wl,--strip-all)
target_link_libraries(BigHeadVPNStartup PRIVATE advapi32 oleaut32 shell32 taskschd)
set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/startup.rc" APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/../packaging/big-head-vpn.ico"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version-resource.rc"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/startup.manifest")
