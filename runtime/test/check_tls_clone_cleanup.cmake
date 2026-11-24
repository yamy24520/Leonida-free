# gta6rt --dry-run, --init and --dry-run again with tiny_tls_clones.exe, which maps three TLS modules and so makes a
# helper clone for one of its DLLs. After each run no clone file of that run's process may remain in the temporary
# folder but its home clone (leonida_tls_host_<pid>_home.dll), which is mapped until the process ends and cannot be
# deleted before; the next run's sweep must delete that one. The process ID comes from the trace's first line. A gta6rt
# running beside this test may sweep the home clone first, so its absence is fine.
if(DEFINED ENV{TMP})
  set(temp "$ENV{TMP}")
else()
  set(temp "$ENV{TEMP}")
endif()
file(TO_CMAKE_PATH "${temp}" temp)
set(previous "")
foreach(mode dry-run init dry-run)
  set(trace "${TRACE_DIR}/tls_clone_cleanup.jsonl")
  file(REMOVE "${trace}")  # --trace only creates a new file
  execute_process(
    COMMAND ${RUNNER} "${LOADER}" --${mode} "${GUEST}" --trace "${trace}"
    TIMEOUT 90
    RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT code STREQUAL "0" OR NOT output MATCHES "3 module\\(s\\) mapped")
    message(FATAL_ERROR "--${mode} returned ${code}\n${output}\n${error}")
  endif()
  file(STRINGS "${trace}" first LIMIT_COUNT 1)
  file(REMOVE "${trace}")
  if(NOT first MATCHES "\"pid\":([0-9]+)")
    message(FATAL_ERROR "--${mode}: no pid in the trace: ${first}")
  endif()
  set(pid "${CMAKE_MATCH_1}")
  file(GLOB left "${temp}/leonida_tls_host_${pid}_*.dll")
  if(left AND NOT left STREQUAL "${temp}/leonida_tls_host_${pid}_home.dll")
    message(FATAL_ERROR "--${mode} (pid ${pid}) left clone files other than its home clone: ${left}")
  endif()
  if(previous)
    file(GLOB swept "${temp}/leonida_tls_host_${previous}_*.dll")
    if(swept)
      message(FATAL_ERROR "the run after pid ${previous} did not delete its clone files: ${swept}")
    endif()
  endif()
  set(previous "${pid}")
endforeach()
