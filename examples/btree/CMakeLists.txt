# A B+tree on PageStore, and kv, a key-value tool built on it. Enable with
# PS_BUILD_EXAMPLES, which ./dev.py sets for --example.

add_library(ps_btree STATIC btree.c)
target_include_directories(ps_btree PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ps_btree PUBLIC pagestore)

add_executable(kv kv.c)
target_link_libraries(kv PRIVATE ps_btree)

ps_add_test(NAME test_btree LABEL example SOURCES test_btree.c)
target_link_libraries(test_btree PRIVATE ps_btree)

add_test(NAME test_kv
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/test_kv.sh $<TARGET_FILE:kv>)
set_tests_properties(test_kv PROPERTIES LABELS example TIMEOUT 300)
if(PS_COVERAGE)
  set_tests_properties(test_kv PROPERTIES
    ENVIRONMENT "LLVM_PROFILE_FILE=${PS_COVERAGE_DIR}/profiles/test_kv-%p.profraw")
endif()
