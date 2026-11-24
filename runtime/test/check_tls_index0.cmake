# gta6rt --run and --init with tiny_tls_ga.exe, an executable built /GA whose thread_local reads go straight to
# ThreadLocalStoragePointer[0]. --run: the game thread and a second thread both read the template value 42 and
# _tls_index is 0. Both modes: the startup trace names gta6rt.exe as the image that held module TLS index 0 and the
# index its block moved to. Then, with TMP and TEMP naming a missing folder, the home clone cannot be written and the
# reservation fails before main: --run and --init refuse the image with that reason and run none of its code, and
# --dry-run maps it with the note. (A TMP longer than MAX_PATH does not get this far: GetTempPathW then skips it.)
foreach(mode run init)
  set(trace "${TRACE_DIR}/tls_index0_${mode}.jsonl")
  file(REMOVE "${trace}")  # --trace only creates a new file
  execute_process(
    COMMAND ${RUNNER} "${LOADER}" --${mode} "${GUEST}" --trace "${trace}"
    TIMEOUT 90
    RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT code STREQUAL "0")
    message(FATAL_ERROR "--${mode} returned ${code}\n${output}\n${error}")
  endif()
  if(mode STREQUAL "run" AND (NOT output MATCHES "tiny_tls_ga: tls_index=0 main=42 thread=42 OK"
      OR NOT output MATCHES "game exited with code 0"))
    message(FATAL_ERROR "--run did not read the TLS from slot 0\n${output}\n${error}")
  endif()
  if(error MATCHES "does not get module TLS index 0")
    message(FATAL_ERROR "--${mode}: the executable was not given index 0\n${error}")
  endif()
  file(READ "${trace}" lines)
  if(NOT lines MATCHES "\"kind\":\"tls_index0\",\"owner\":\"gta6rt.exe\",\"moved_to\":[1-9][0-9]*}")
    message(FATAL_ERROR "--${mode}: no tls_index0 event naming gta6rt.exe in the trace\n${lines}")
  endif()
  file(REMOVE "${trace}")
endforeach()

set(missing "${TRACE_DIR}/leonida_no_such_temp")
file(REMOVE_RECURSE "${missing}")
foreach(mode run init dry-run)
  execute_process(
    COMMAND ${CMAKE_COMMAND} -E env "TMP=${missing}" "TEMP=${missing}" ${RUNNER} "${LOADER}" --${mode} "${GUEST}"
    TIMEOUT 90
    RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error)
  set(reason "module TLS index 0 \\(the home clone of gta6rt_tls_host.dll cannot be loaded")
  if(mode STREQUAL "dry-run")
    if(NOT code STREQUAL "0" OR NOT error MATCHES "does not get ${reason}")
      message(FATAL_ERROR "--dry-run without the reservation returned ${code}\n${output}\n${error}")
    endif()
  elseif(NOT code STREQUAL "1" OR NOT error MATCHES "cannot have ${reason}" OR output MATCHES "running ")
    message(FATAL_ERROR "--${mode} without the reservation returned ${code}\n${output}\n${error}")
  endif()
endforeach()
