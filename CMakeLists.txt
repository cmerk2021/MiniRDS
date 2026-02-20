cmake_minimum_required(VERSION 3.14)
project(minirds C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Version
set(MINIRDS_VERSION "1.0")

# Options
option(RDS2 "Enable RDS2 capabilities" ON)
option(RDS2_QUADRATURE_CARRIER "Shift RDS2 stream carriers by 90/180/270 degrees" ON)
option(RDS2_SYMBOL_SHIFTING "RDS2 symbol shifting to reduce peak amplitude" ON)
option(RDS2_DEBUG "RDS2 debugging" OFF)
option(RBDS "NRSC RBDS (FCC) mode" ON)
option(STATIC_LIBSAMPLERATE "Use a static libsamplerate library" OFF)

# Source files
set(SOURCES
    src/minirds.c
    src/waveforms.c
    src/rds.c
    src/fm_mpx.c
    src/control_pipe.c
    src/osc.c
    src/resampler.c
    src/modulator.c
    src/lib.c
    src/net.c
    src/ascii_cmd.c
)

if(RDS2)
    list(APPEND SOURCES src/rds2.c)
endif()

# On Windows, bundle getopt
if(WIN32)
    list(APPEND SOURCES src/getopt.c)
endif()

add_executable(minirds ${SOURCES})

# Compiler definitions
target_compile_definitions(minirds PRIVATE VERSION="${MINIRDS_VERSION}")

if(RDS2)
    target_compile_definitions(minirds PRIVATE RDS2)
    if(RDS2_QUADRATURE_CARRIER)
        target_compile_definitions(minirds PRIVATE RDS2_QUADRATURE_CARRIER)
    endif()
    if(RDS2_SYMBOL_SHIFTING)
        target_compile_definitions(minirds PRIVATE RDS2_SYMBOL_SHIFTING)
    endif()
endif()

if(RDS2_DEBUG)
    target_compile_definitions(minirds PRIVATE RDS2_DEBUG)
endif()

if(RBDS)
    target_compile_definitions(minirds PRIVATE RBDS)
endif()

# Compiler flags
if(MSVC)
    target_compile_options(minirds PRIVATE /W3 /O2)
    # Disable common MSVC warnings for POSIX function names
    target_compile_definitions(minirds PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_DEPRECATE
        _WINSOCK_DEPRECATED_NO_WARNINGS
    )
else()
    target_compile_options(minirds PRIVATE -Wall -Wextra -pedantic -O2)
endif()

# Platform-specific linking
if(WIN32)
    target_link_libraries(minirds PRIVATE ws2_32)
else()
    target_link_libraries(minirds PRIVATE m pthread)
endif()

# libao
find_path(AO_INCLUDE_DIR ao/ao.h
    HINTS ${CMAKE_PREFIX_PATH}/include
)
find_library(AO_LIBRARY NAMES ao libao
    HINTS ${CMAKE_PREFIX_PATH}/lib
)

if(AO_INCLUDE_DIR AND AO_LIBRARY)
    target_include_directories(minirds PRIVATE ${AO_INCLUDE_DIR})
    target_link_libraries(minirds PRIVATE ${AO_LIBRARY})
else()
    message(FATAL_ERROR "libao not found. Please install libao or set CMAKE_PREFIX_PATH.")
endif()

# libsamplerate
if(STATIC_LIBSAMPLERATE)
    find_library(SAMPLERATE_LIBRARY NAMES samplerate libsamplerate
        HINTS ${CMAKE_PREFIX_PATH}/lib
        NO_DEFAULT_PATH
    )
    find_path(SAMPLERATE_INCLUDE_DIR samplerate.h
        HINTS ${CMAKE_PREFIX_PATH}/include
    )
else()
    find_path(SAMPLERATE_INCLUDE_DIR samplerate.h
        HINTS ${CMAKE_PREFIX_PATH}/include
    )
    find_library(SAMPLERATE_LIBRARY NAMES samplerate libsamplerate
        HINTS ${CMAKE_PREFIX_PATH}/lib
    )
endif()

if(SAMPLERATE_INCLUDE_DIR AND SAMPLERATE_LIBRARY)
    target_include_directories(minirds PRIVATE ${SAMPLERATE_INCLUDE_DIR})
    target_link_libraries(minirds PRIVATE ${SAMPLERATE_LIBRARY})
else()
    message(FATAL_ERROR "libsamplerate not found. Please install libsamplerate or set CMAKE_PREFIX_PATH.")
endif()

# Install
install(TARGETS minirds RUNTIME DESTINATION bin)
