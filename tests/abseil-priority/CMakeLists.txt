cmake_minimum_required(VERSION 3.25)
project(AbseilSandboxPriorityTest LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory("${ABSEIL_SOURCE}" abseil EXCLUDE_FROM_ALL)
add_executable(priority-test priority-test.c++)
target_link_libraries(priority-test PRIVATE absl::synchronization)
target_link_options(priority-test PRIVATE -Wl,--wrap=pthread_getschedparam)
enable_testing()
foreach(error 0 1 38 3)
  add_test(NAME priority-${error} COMMAND priority-test ${error})
endforeach()
set_tests_properties(priority-0 priority-1 priority-38 PROPERTIES
  FAIL_REGULAR_EXPRESSION "pthread_getschedparam failed")
set_tests_properties(priority-3 PROPERTIES
  FAIL_REGULAR_EXPRESSION "unexpected probe count"
  PASS_REGULAR_EXPRESSION "pthread_getschedparam failed: 3")
