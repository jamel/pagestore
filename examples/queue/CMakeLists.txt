# A durable message queue on PageStore, and mq, a tool built on it. Enable
# with PS_BUILD_EXAMPLES, which ./dev.py sets for --example.

add_library(ps_queue STATIC radix.c msglog.c mq.c)
target_include_directories(ps_queue PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ps_queue PUBLIC pagestore)

add_executable(mq mqtool.c)
target_link_libraries(mq PRIVATE ps_queue)

ps_add_test(NAME test_msglog LABEL example SOURCES test_msglog.c)
target_link_libraries(test_msglog PRIVATE ps_queue)

ps_add_test(NAME test_mq LABEL example SOURCES test_mq.c)
target_link_libraries(test_mq PRIVATE ps_queue)

add_test(NAME test_mq_tool
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/test_mq.sh $<TARGET_FILE:mq>)
set_tests_properties(test_mq_tool PROPERTIES LABELS example TIMEOUT 300)
if(PS_COVERAGE)
  set_tests_properties(test_mq_tool PROPERTIES
    ENVIRONMENT "LLVM_PROFILE_FILE=${PS_COVERAGE_DIR}/profiles/test_mq_tool-%p.profraw")
endif()
