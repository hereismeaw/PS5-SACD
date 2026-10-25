file(REMOVE "${OUTPUT}" "${WAV_OUTPUT}")

execute_process(
    COMMAND "${SACDINFO}" "${INPUT}"
    RESULT_VARIABLE info_result
    OUTPUT_VARIABLE info_output
    ERROR_VARIABLE info_error)
if(NOT info_result EQUAL 0)
    message(FATAL_ERROR "sacdinfo failed: ${info_error}")
endif()
if(NOT info_output MATCHES "Tracks: 1"
    OR NOT info_output MATCHES "LSN 600..604"
    OR NOT info_output MATCHES "Album: Album Title"
    OR NOT info_output MATCHES "Track 1: Track One")
    message(FATAL_ERROR "sacdinfo did not report the fixture track extent: ${info_output}")
endif()

execute_process(
    COMMAND "${SACDEXTRACT}" "${INPUT}" 1 "${OUTPUT}"
    RESULT_VARIABLE extract_result
    OUTPUT_VARIABLE extract_output
    ERROR_VARIABLE extract_error)
if(NOT extract_result EQUAL 0)
    message(FATAL_ERROR "sacdextract failed: ${extract_error}")
endif()
if(NOT EXISTS "${OUTPUT}")
    message(FATAL_ERROR "sacdextract reported success without creating DSDIFF output")
endif()

execute_process(
    COMMAND "${DSD2WAV}" "${OUTPUT}" "${WAV_OUTPUT}"
    RESULT_VARIABLE convert_result
    OUTPUT_VARIABLE convert_output
    ERROR_VARIABLE convert_error)
if(NOT convert_result EQUAL 0)
    message(FATAL_ERROR "dsd2wav rejected extracted DSDIFF: ${convert_error}")
endif()
if(NOT EXISTS "${WAV_OUTPUT}")
    message(FATAL_ERROR "dsd2wav reported success without creating WAV output")
endif()

file(SIZE "${OUTPUT}" dsdiff_size)
file(SIZE "${WAV_OUTPUT}" wav_size)
if(dsdiff_size LESS 9000 OR wav_size LESS 1000)
    message(FATAL_ERROR "CLI outputs are unexpectedly small (${dsdiff_size}, ${wav_size})")
endif()

file(REMOVE "${OUTPUT}" "${WAV_OUTPUT}")
