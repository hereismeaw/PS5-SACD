cmake_minimum_required(VERSION 3.25)

project(ps5_sacd_player VERSION 0.1.0 LANGUAGES CXX)

# Host targets use C++23. The PS5 payload is built separately with the
# SDK-specific linker bridge in tools/ps5/build-windows.ps1.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_library(ps5_sacd_host_warnings INTERFACE)
if(MSVC)
    target_compile_options(ps5_sacd_host_warnings INTERFACE /W4 /permissive-)
else()
    target_compile_options(ps5_sacd_host_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
    )
endif()

add_library(sacd_dsf
    src/formats/dsf/parser.cpp
)
target_include_directories(sacd_dsf
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(sacd_dsf PRIVATE ps5_sacd_host_warnings)

add_library(sacd_dsdiff
    src/formats/dsdiff/parser.cpp
)
target_include_directories(sacd_dsdiff
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(sacd_dsdiff PRIVATE ps5_sacd_host_warnings)

add_library(sacd_iso
    src/formats/sacd_iso/parser.cpp
)
target_include_directories(sacd_iso
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(sacd_iso PRIVATE ps5_sacd_host_warnings)

add_library(sacd_audio
    src/audio/dsd_to_pcm.cpp
)
target_include_directories(sacd_audio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(sacd_audio PRIVATE ps5_sacd_host_warnings)

add_library(sacd_wav
    src/audio/dsd_to_wav.cpp
)
target_include_directories(sacd_wav
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(sacd_wav
    PUBLIC sacd_dsf sacd_dsdiff sacd_audio
    PRIVATE ps5_sacd_host_warnings
)

add_executable(dsd2wav
    src/cli/dsd2wav.cpp
)
target_link_libraries(dsd2wav PRIVATE sacd_wav ps5_sacd_host_warnings)

if(BUILD_TESTING)
    add_executable(dsf_parser_tests
        tests/dsf/parser_tests.cpp
    )
    target_link_libraries(dsf_parser_tests PRIVATE sacd_dsf ps5_sacd_host_warnings)
    add_test(NAME dsf_parser_tests COMMAND dsf_parser_tests)

    add_executable(dsdiff_parser_tests
        tests/dsdiff/parser_tests.cpp
    )
    target_link_libraries(dsdiff_parser_tests PRIVATE sacd_dsdiff ps5_sacd_host_warnings)
    add_test(NAME dsdiff_parser_tests COMMAND dsdiff_parser_tests)

    add_executable(sacd_iso_parser_tests
        tests/sacd_iso/parser_tests.cpp
    )
    target_link_libraries(sacd_iso_parser_tests PRIVATE sacd_iso ps5_sacd_host_warnings)
    add_test(NAME sacd_iso_parser_tests COMMAND sacd_iso_parser_tests)

    add_executable(dsd_to_pcm_tests
        tests/audio/dsd_to_pcm_tests.cpp
    )
    target_link_libraries(dsd_to_pcm_tests PRIVATE sacd_audio ps5_sacd_host_warnings)
    add_test(NAME dsd_to_pcm_tests COMMAND dsd_to_pcm_tests)

    add_executable(dsd_to_wav_tests
        tests/audio/dsd_to_wav_tests.cpp
    )
    target_link_libraries(dsd_to_wav_tests PRIVATE sacd_wav ps5_sacd_host_warnings)
    add_test(NAME dsd_to_wav_tests COMMAND dsd_to_wav_tests)
    add_test(NAME dsd2wav_help COMMAND dsd2wav --help)
endif()
