set(PS_TESTSUPPORT_SOURCES
  support/test.c
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
  if(NOT T_LABEL MATCHES "^(unit|model|crash|determinism|stress)$")
    message(FATAL_ERROR "ps_add_test: bad LABEL '${T_LABEL}'")
  endif()
  add_executable(${T_NAME} ${T_SOURCES})
  target_include_directories(${T_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/tests/support)
  target_link_libraries(${T_NAME} PRIVATE ps_testsupport pagestore Threads::Threads)
  add_test(NAME ${T_NAME} COMMAND ${T_NAME})
  if(T_LABEL STREQUAL "crash" OR T_LABEL STREQUAL "stress")
    set(timeout 900)
  else()
    set(timeout 120)
  endif()
  set_tests_properties(${T_NAME} PROPERTIES
    LABELS ${T_LABEL}
    TIMEOUT ${timeout}
    ENVIRONMENT "${PS_TEST_ENV}")
endfunction()

ps_add_test(NAME test_crc32c LABEL unit SOURCES unit/test_crc32c.c)
ps_add_test(NAME test_bits   LABEL unit SOURCES unit/test_bits.c)
ps_add_test(NAME test_compiler LABEL unit SOURCES unit/test_compiler.c)
ps_add_test(NAME test_vfs_posix LABEL unit SOURCES unit/test_vfs_posix.c)
