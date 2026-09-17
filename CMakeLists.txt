cmake_minimum_required(VERSION 3.18)
project(tinyspot C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(TP ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# Local fixes to pinned third-party code live in patches/cspot/*.patch. They
# are applied in order once; applied names are recorded in a stamp file in the
# submodule (patches that touch the same files can't be reverse-checked alone).
file(GLOB CSPOT_PATCHES ${CMAKE_CURRENT_SOURCE_DIR}/patches/cspot/*.patch)
list(SORT CSPOT_PATCHES)
set(PATCH_STAMP ${TP}/cspot/.tinyspot-patches)
set(APPLIED "")
if(EXISTS ${PATCH_STAMP})
  file(STRINGS ${PATCH_STAMP} APPLIED)
endif()
foreach(p ${CSPOT_PATCHES})
  get_filename_component(name ${p} NAME)
  if(NOT name IN_LIST APPLIED)
    execute_process(COMMAND git apply --whitespace=nowarn ${p}
                    WORKING_DIRECTORY ${TP}/cspot RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "failed to apply ${p}")
    endif()
    file(APPEND ${PATCH_STAMP} "${name}\n")
    message(STATUS "applied ${name}")
  endif()
endforeach()

# ---------------------------------------------------------------------------
# Size-first compiler flags (release). -ffunction/data-sections + --gc-sections
# drop everything bell/cspot compile but we never call.
# ---------------------------------------------------------------------------
add_compile_options(-ffunction-sections -fdata-sections -fvisibility=hidden -Wno-everything)
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_compile_options(-Os)
endif()

# ---------------------------------------------------------------------------
# mbedTLS (TLS for apresolve / login5 / CDN, plus AES/DH/SHA for the AP)
# ---------------------------------------------------------------------------
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(${TP}/mbedtls ${CMAKE_BINARY_DIR}/mbedtls EXCLUDE_FROM_ALL)

# bell's FindMbedTLS uses find_path/find_library: pre-seeding the cache makes
# those calls no-ops and links the in-tree targets instead.
set(MBEDTLS_INCLUDE_DIRS ${TP}/mbedtls/include CACHE PATH "" FORCE)
set(MBEDTLS_LIBRARY mbedtls CACHE STRING "" FORCE)
set(MBEDX509_LIBRARY mbedx509 CACHE STRING "" FORCE)
set(MBEDCRYPTO_LIBRARY mbedcrypto CACHE STRING "" FORCE)

# ---------------------------------------------------------------------------
# bell + cspot: only what a Connect receiver needs
# ---------------------------------------------------------------------------
set(BELL_DISABLE_CODECS ON CACHE BOOL "" FORCE)   # cspot calls tremor directly
set(BELL_DISABLE_SINKS ON CACHE BOOL "" FORCE)    # our own OpenSL ES sink
set(BELL_DISABLE_MQTT ON CACHE BOOL "" FORCE)
set(BELL_ONLY_CJSON ON CACHE BOOL "" FORCE)       # no nlohmann template bloat
set(BELL_DISABLE_FMT ON CACHE BOOL "" FORCE)
set(BELL_DISABLE_REGEX ON CACHE BOOL "" FORCE)
set(BELL_DISABLE_AVAHI ON CACHE BOOL "" FORCE)
set(PROTOBUF_PROTOC_EXECUTABLE ${CMAKE_CURRENT_SOURCE_DIR}/tools/protoc CACHE FILEPATH "" FORCE)
add_subdirectory(${TP}/cspot/cspot ${CMAKE_BINARY_DIR}/cspot EXCLUDE_FROM_ALL)
target_compile_definitions(bell PRIVATE USE_IPV6)  # civetweb: dual-stack zeroconf

# ---------------------------------------------------------------------------
# libtinyspot.so
# ---------------------------------------------------------------------------
add_library(tinyspot SHARED
  native/jni_bridge.cpp
  native/CspotPlayer.cpp
  native/AndroidAudioSink.cpp
  native/SpotifyLibrary.cpp
)
target_include_directories(tinyspot PRIVATE native)
target_compile_options(tinyspot PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(tinyspot PRIVATE cspot log OpenSLES)
target_link_options(tinyspot PRIVATE -Wl,--gc-sections -Wl,--exclude-libs,ALL)
