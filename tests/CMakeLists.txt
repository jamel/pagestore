set(PS_TESTSUPPORT_SOURCES
  support/test.c
  support/fault_vfs.c
  support/mem_backend.c
  support/model.c
  support/workload.c
)

add_library(ps_testsupport STATIC ${PS_TESTSUPPORT_SOURCES})
target_include_directories(ps_testsupport PUBLIC
  ${PROJECT_SOURCE_DIR}/include
  ${PROJECT_SOURCE_DIR}/src
  ${PROJECT_SOURCE_DIR}/tests/support)
target_link_libraries(ps_testsupport PUBLIC pagestore Threads::Threads)

if(APPLE)
  set(PS_ASAN_OPTIONS "ASAN_OPTIONS=detect_leaks=0:abort_on_error=1")
else()
  set(PS_ASAN_OPTIONS "ASAN_OPTIONS=detect_leaks=1:abort_on_error=1")
endif()
set(PS_TEST_ENV
  "${PS_ASAN_OPTIONS}"
  "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1"
  "TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1")

# LABEL selects the suite for commands like `./dev.py test --label unit`.
function(ps_add_test)
  cmake_parse_arguments(T "" "NAME;LABEL" "SOURCES" ${ARGN})
  if(NOT T_NAME OR NOT T_LABEL OR NOT T_SOURCES)
    message(FATAL_ERROR "ps_add_test: NAME, LABEL and SOURCES are required")
  endif()
  if(NOT T_LABEL MATCHES "^(unit|model|crash|determinism|stress|example)$")
    message(FATAL_ERROR "ps_add_test: bad LABEL '${T_LABEL}'")
  endif()
  add_executable(${T_NAME} ${T_SOURCES})
  target_include_directories(${T_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/tests/support)
  target_link_libraries(${T_NAME} PRIVATE ps_testsupport pagestore Threads::Threads)
  add_test(NAME ${T_NAME} COMMAND ${T_NAME})
  if(T_LABEL STREQUAL "crash" OR T_LABEL STREQUAL "stress" OR T_LABEL STREQUAL "model")
    set(timeout 900)
  else()
    set(timeout 120)
  endif()
  set(env ${PS_TEST_ENV})
  if(PS_COVERAGE)
    # %p gives each process its own file, so repeated runs add up.
    list(APPEND env "LLVM_PROFILE_FILE=${PS_COVERAGE_DIR}/profiles/${T_NAME}-%p.profraw")
  endif()
  set_tests_properties(${T_NAME} PROPERTIES
    LABELS ${T_LABEL}
    TIMEOUT ${timeout}
    ENVIRONMENT "${env}")
endfunction()

ps_add_test(NAME test_crc32c LABEL unit SOURCES unit/test_crc32c.c)
ps_add_test(NAME test_bits   LABEL unit SOURCES unit/test_bits.c)
ps_add_test(NAME test_compiler LABEL unit SOURCES unit/test_compiler.c)
ps_add_test(NAME test_logger LABEL unit SOURCES unit/test_logger.c)
ps_add_test(NAME test_vfs_posix LABEL unit SOURCES unit/test_vfs_posix.c)
ps_add_test(NAME test_fault_vfs LABEL unit SOURCES unit/test_fault_vfs.c)
ps_add_test(NAME test_format LABEL unit SOURCES unit/test_format.c)
ps_add_test(NAME test_backend_file LABEL unit SOURCES unit/test_backend_file.c)
ps_add_test(NAME test_frames LABEL unit SOURCES unit/test_frames.c)
ps_add_test(NAME test_ptable LABEL unit SOURCES unit/test_ptable.c)
ps_add_test(NAME test_allocator LABEL unit SOURCES unit/test_allocator.c)
ps_add_test(NAME test_ops LABEL unit SOURCES unit/test_ops.c)
ps_add_test(NAME test_checkpoint LABEL unit SOURCES unit/test_checkpoint.c)
ps_add_test(NAME test_snapshot LABEL unit SOURCES unit/test_snapshot.c)
ps_add_test(NAME test_memory LABEL unit SOURCES unit/test_memory.c)
ps_add_test(NAME test_ckpt_thread LABEL unit SOURCES unit/test_ckpt_thread.c)
ps_add_test(NAME test_store_file LABEL unit SOURCES unit/test_store_file.c)
ps_add_test(NAME test_crash_store LABEL crash SOURCES crash/test_crash_store.c)
ps_add_test(NAME test_crash_wal LABEL crash SOURCES crash/test_crash_wal.c)
ps_add_test(NAME test_wal_write LABEL unit SOURCES unit/test_wal_write.c)
ps_add_test(NAME test_wal_open LABEL unit SOURCES unit/test_wal_open.c)
ps_add_test(NAME test_recover LABEL unit SOURCES unit/test_recover.c)
ps_add_test(NAME test_model LABEL model SOURCES model/test_model.c)
ps_add_test(NAME test_determinism LABEL determinism SOURCES determinism/test_determinism.c)
ps_add_test(NAME test_stress LABEL stress SOURCES stress/test_stress.c)
ps_add_test(NAME test_pressure LABEL stress SOURCES stress/test_pressure.c)
