cmake_minimum_required(VERSION 3.20)
project(pagestore LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

set(PS_SANITIZE "" CACHE STRING "Sanitizer: address | thread | memory (empty = none)")
set_property(CACHE PS_SANITIZE PROPERTY STRINGS "" address thread memory)

add_compile_options(-Wall -Wextra -Werror)

if(PS_SANITIZE STREQUAL "address")
  set(PS_SAN_FLAGS -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
elseif(PS_SANITIZE STREQUAL "thread")
  set(PS_SAN_FLAGS -fsanitize=thread)
elseif(PS_SANITIZE STREQUAL "memory")
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "PS_SANITIZE=memory requires clang")
  endif()
  set(PS_SAN_FLAGS -fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer)
elseif(NOT PS_SANITIZE STREQUAL "")
  message(FATAL_ERROR "Unknown PS_SANITIZE value '${PS_SANITIZE}'")
endif()
if(PS_SAN_FLAGS)
  add_compile_options(${PS_SAN_FLAGS})
  add_link_options(${PS_SAN_FLAGS})
endif()

# Coverage uses clang's source-based coverage. Each test writes a profile to
# PS_COVERAGE_DIR/profiles. Then build these targets:
#
#   coverage-reset   remove profiles left by earlier test runs
#   coverage-report  merge the profiles, print a summary, and write an HTML
#                    report to PS_COVERAGE_DIR/html
#
# For example: ./dev.py test --coverage wal
option(PS_COVERAGE "Build with clang source-based coverage" OFF)
if(PS_COVERAGE)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "PS_COVERAGE requires clang")
  endif()
  add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
  add_link_options(-fprofile-instr-generate)

  # The tools must match the compiler's version, so look next to it first.
  get_filename_component(ps_llvm_bin "${CMAKE_C_COMPILER}" REALPATH)
  get_filename_component(ps_llvm_bin "${ps_llvm_bin}" DIRECTORY)
  string(REGEX MATCH "^[0-9]+" ps_llvm_major "${CMAKE_C_COMPILER_VERSION}")
  find_program(PS_LLVM_PROFDATA NAMES llvm-profdata-${ps_llvm_major} llvm-profdata
    HINTS "${ps_llvm_bin}" REQUIRED)
  find_program(PS_LLVM_COV NAMES llvm-cov-${ps_llvm_major} llvm-cov
    HINTS "${ps_llvm_bin}" REQUIRED)

  # The coverage targets are defined at the end, after every test.
  set(PS_COVERAGE_DIR ${PROJECT_BINARY_DIR}/coverage)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_compile_definitions(_GNU_SOURCE)
elseif(APPLE)
  add_compile_definitions(_DARWIN_C_SOURCE)
endif()

set(PS_SOURCES
  src/crc32c.c
  src/logger.c
  src/bitmap.c
  src/path.c
  src/vfs_posix.c
  src/format.c
  src/backend_file.c
  src/frames.c
  src/ptable.c
  src/allocator.c
  src/store.c
  src/op.c
  src/pagein.c
  src/versions.c
  src/mem.c
  src/checkpoint.c
  src/snapshot.c
  src/wal.c
  src/recover.c
)

add_library(pagestore STATIC ${PS_SOURCES})
target_include_directories(pagestore
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pagestore PUBLIC Threads::Threads)

enable_testing()
add_subdirectory(tests)

# Examples use only the public API. Their tests carry the "example" label.
# ./dev.py passes this on every configure: ON with --example, OFF without.
option(PS_BUILD_EXAMPLES "Build the examples and their tests" OFF)
if(PS_BUILD_EXAMPLES)
  add_subdirectory(examples/btree)
  add_subdirectory(examples/queue)
endif()

if(PS_COVERAGE)
  # Read the library so every source file shows up, even when the selected
  # tests never link it. The test programs add the header functions they
  # inline. Only library files are reported, in sorted order. Each test that
  # did not run adds a "mismatched data" warning for its main().
  set(ps_profdata ${PS_COVERAGE_DIR}/merged.profdata)
  set(ps_cov_args $<TARGET_FILE:pagestore>)
  get_property(ps_cov_tests GLOBAL PROPERTY PS_COVERAGE_TESTS)
  foreach(t IN LISTS ps_cov_tests)
    list(APPEND ps_cov_args -object=$<TARGET_FILE:${t}>)
  endforeach()
  file(GLOB ps_cov_sources
    ${PROJECT_SOURCE_DIR}/include/pagestore/*.h
    ${PROJECT_SOURCE_DIR}/src/*.[ch])
  list(APPEND ps_cov_args -instr-profile=${ps_profdata}
    -show-region-summary=false -show-branch-summary=false ${ps_cov_sources})

  add_custom_target(coverage-reset
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${PS_COVERAGE_DIR}/profiles
    COMMENT "Removing old coverage profiles"
    VERBATIM)
  add_custom_target(coverage-report
    COMMAND ${PS_LLVM_PROFDATA} merge -sparse -o ${ps_profdata} ${PS_COVERAGE_DIR}/profiles
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${PS_COVERAGE_DIR}/html
    COMMAND ${PS_LLVM_COV} show ${ps_cov_args} -format=html
      -output-dir=${PS_COVERAGE_DIR}/html -project-title=PageStore
    COMMAND ${PS_LLVM_COV} report ${ps_cov_args}
    COMMAND ${CMAKE_COMMAND} -E echo "HTML report: ${PS_COVERAGE_DIR}/html/index.html"
    COMMENT "Writing coverage report"
    USES_TERMINAL
    VERBATIM)
endif()
