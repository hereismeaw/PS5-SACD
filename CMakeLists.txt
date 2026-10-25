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

add_library(sacd_dsdiff_writer
    src/audio/dsdiff_writer.cpp
)
target_include_directories(sacd_dsdiff_writer
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(sacd_dsdiff_writer PRIVATE ps5_sacd_host_warnings)

add_library(sacd_iso_extractor
    src/formats/sacd_iso/extractor.cpp
)
target_include_directories(sacd_iso_extractor
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(sacd_iso_extractor
    PUBLIC sacd_iso
    PRIVATE sacd_dsdiff_writer ps5_sacd_host_warnings
)

add_library(sacd_iso
    src/formats/sacd_iso/parser.cpp
    src/formats/sacd_iso/audio_frames.cpp
    src/formats/sacd_iso/text.cpp
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

add_executable(sacdinfo
    src/cli/sacdinfo.cpp
)
target_link_libraries(sacdinfo PRIVATE sacd_iso ps5_sacd_host_warnings)

add_executable(sacdextract
    src/cli/sacdextract.cpp
)
target_link_libraries(sacdextract PRIVATE sacd_iso_extractor ps5_sacd_host_warnings)

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

    add_executable(sacd_iso_audio_frame_tests
        tests/sacd_iso/audio_frames_tests.cpp
    )
    target_link_libraries(sacd_iso_audio_frame_tests PRIVATE sacd_iso ps5_sacd_host_warnings)
    add_test(NAME sacd_iso_audio_frame_tests COMMAND sacd_iso_audio_frame_tests)

    add_executable(dsdiff_writer_tests
        tests/audio/dsdiff_writer_tests.cpp
    )
    target_link_libraries(dsdiff_writer_tests PRIVATE
        sacd_dsdiff_writer sacd_dsdiff ps5_sacd_host_warnings)
    add_test(NAME dsdiff_writer_tests COMMAND dsdiff_writer_tests)

    add_executable(sacd_iso_extractor_tests
        tests/sacd_iso/extractor_tests.cpp
    )
    target_link_libraries(sacd_iso_extractor_tests PRIVATE
        sacd_iso_extractor sacd_dsdiff sacd_wav ps5_sacd_host_warnings)
    add_test(NAME sacd_iso_extractor_tests COMMAND sacd_iso_extractor_tests)

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
    add_test(NAME sacdinfo_help COMMAND sacdinfo --help)
    add_test(NAME sacdextract_help COMMAND sacdextract --help)

    set(SACD_CLI_FIXTURE_PATH "${CMAKE_CURRENT_BINARY_DIR}/sacd_cli_fixture.iso")
    add_test(NAME sacd_cli_fixture
        COMMAND sacd_iso_extractor_tests --make-fixture "${SACD_CLI_FIXTURE_PATH}")
    set_tests_properties(sacd_cli_fixture PROPERTIES FIXTURES_SETUP sacd_cli_fixture_image)
    add_test(NAME sacd_cli_end_to_end
        COMMAND ${CMAKE_COMMAND}
            "-DSACDINFO=$<TARGET_FILE:sacdinfo>"
            "-DSACDEXTRACT=$<TARGET_FILE:sacdextract>"
            "-DDSD2WAV=$<TARGET_FILE:dsd2wav>"
            "-DINPUT=${SACD_CLI_FIXTURE_PATH}"
            "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/sacd_cli_track.dff"
            "-DWAV_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/sacd_cli_track.wav"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/sacd_cli_end_to_end.cmake")
    set_tests_properties(sacd_cli_end_to_end PROPERTIES FIXTURES_REQUIRED sacd_cli_fixture_image)
endif()
