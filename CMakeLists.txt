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

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_compile_definitions(_GNU_SOURCE)
elseif(APPLE)
  add_compile_definitions(_DARWIN_C_SOURCE)
endif()

set(PS_SOURCES
  src/crc32c.c
  src/bitmap.c
  src/vfs_posix.c
  src/format.c
  src/backend_file.c
  src/frames.c
)

add_library(pagestore STATIC ${PS_SOURCES})
target_include_directories(pagestore
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(pagestore PUBLIC Threads::Threads)

enable_testing()
add_subdirectory(tests)
