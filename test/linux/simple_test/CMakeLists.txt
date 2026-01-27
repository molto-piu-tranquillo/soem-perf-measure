
set(SOURCES simple_test.c)
# add_executable(simple_test ${SOURCES})
# target_link_libraries(simple_test soem)

add_executable(simple_test simple_test.c)
target_link_libraries(simple_test soem)

add_executable(latency_test latency_test.c)
target_link_libraries(latency_test soem)

add_executable(test test.c)
target_link_libraries(test soem)

add_executable(cycle_test cycle_test.c)
target_link_libraries(cycle_test soem)

add_executable(cycle_test_2 cycle_test_2.c)
target_link_libraries(cycle_test_2 soem)

install(TARGETS simple_test latency_test test cycle_test cycle_test_2 DESTINATION bin)
