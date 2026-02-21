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

# GUI option (Windows only)
option(BUILD_GUI "Build the Windows GUI application" ON)

# --------------------------------------------------------------------------
# Find dependencies (shared by CLI and GUI)
# --------------------------------------------------------------------------

# libao
find_path(AO_INCLUDE_DIR ao/ao.h
    HINTS ${CMAKE_PREFIX_PATH}/include
)
find_library(AO_LIBRARY NAMES ao libao
    HINTS ${CMAKE_PREFIX_PATH}/lib
)

if(NOT AO_INCLUDE_DIR OR NOT AO_LIBRARY)
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

if(NOT SAMPLERATE_INCLUDE_DIR OR NOT SAMPLERATE_LIBRARY)
    message(FATAL_ERROR "libsamplerate not found. Please install libsamplerate or set CMAKE_PREFIX_PATH.")
endif()

# --------------------------------------------------------------------------
# Core library (shared sources for both CLI and GUI)
# --------------------------------------------------------------------------

set(CORE_SOURCES
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
    list(APPEND CORE_SOURCES src/rds2.c)
endif()

add_library(minirds_core STATIC ${CORE_SOURCES})

# Compile definitions (PUBLIC so consumers also get them)
target_compile_definitions(minirds_core PUBLIC VERSION="${MINIRDS_VERSION}")

if(RDS2)
    target_compile_definitions(minirds_core PUBLIC RDS2)
    if(RDS2_QUADRATURE_CARRIER)
        target_compile_definitions(minirds_core PUBLIC RDS2_QUADRATURE_CARRIER)
    endif()
    if(RDS2_SYMBOL_SHIFTING)
        target_compile_definitions(minirds_core PUBLIC RDS2_SYMBOL_SHIFTING)
    endif()
endif()

if(RDS2_DEBUG)
    target_compile_definitions(minirds_core PUBLIC RDS2_DEBUG)
endif()

if(RBDS)
    target_compile_definitions(minirds_core PUBLIC RBDS)
endif()

# Compiler flags
if(MSVC)
    target_compile_options(minirds_core PRIVATE /W3 /O2)
    target_compile_definitions(minirds_core PUBLIC
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_DEPRECATE
        _WINSOCK_DEPRECATED_NO_WARNINGS
    )
else()
    target_compile_options(minirds_core PRIVATE -Wall -Wextra -pedantic -O2)
endif()

# Include directories (PUBLIC so consumers get them)
target_include_directories(minirds_core PUBLIC
    ${AO_INCLUDE_DIR}
    ${SAMPLERATE_INCLUDE_DIR}
)

# Core library dependencies
target_link_libraries(minirds_core PUBLIC ${AO_LIBRARY} ${SAMPLERATE_LIBRARY})
if(WIN32)
    target_link_libraries(minirds_core PUBLIC ws2_32)
else()
    target_link_libraries(minirds_core PUBLIC m)
endif()

# --------------------------------------------------------------------------
# CLI executable: minirds
# --------------------------------------------------------------------------

set(CLI_SOURCES src/minirds.c)
if(WIN32)
    list(APPEND CLI_SOURCES src/getopt.c)
endif()

add_executable(minirds ${CLI_SOURCES})
target_link_libraries(minirds PRIVATE minirds_core)

if(NOT WIN32)
    target_link_libraries(minirds PRIVATE pthread)
endif()

if(NOT MSVC)
    target_compile_options(minirds PRIVATE -Wall -Wextra -pedantic -O2)
endif()

# --------------------------------------------------------------------------
# GUI executable: minirds_gui (Windows only)
# --------------------------------------------------------------------------

if(WIN32 AND BUILD_GUI)
    add_executable(minirds_gui WIN32 src/minirds_gui.c)
    target_link_libraries(minirds_gui PRIVATE
        minirds_core
        winmm
        comctl32
        comdlg32
    )

    if(NOT MSVC)
        target_compile_options(minirds_gui PRIVATE -Wall -Wextra -pedantic -O2)
    endif()

    install(TARGETS minirds_gui RUNTIME DESTINATION bin)
endif()

# --------------------------------------------------------------------------
# Install
# --------------------------------------------------------------------------

install(TARGETS minirds RUNTIME DESTINATION bin)
