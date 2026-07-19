find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(daichod_unit_tests
  unit/worker_test.cpp
  unit/error_test.cpp
  unit/map_test.cpp
  unit/journal_test.cpp
  unit/mutation_test.cpp)
target_link_libraries(daichod_unit_tests
  PRIVATE daichod_core GTest::gtest_main)
gtest_discover_tests(daichod_unit_tests)

# Integration tests drive the real daemon over its Unix socket against a
# fresh sqlite3 book; the binaries' paths reach the fixture via environment.
add_executable(daichod_integration_tests
  integration/session_test.cpp)
target_link_libraries(daichod_integration_tests
  PRIVATE daichod_proto GTest::gtest_main)
add_dependencies(daichod_integration_tests daichod daichod-mkbook)
gtest_discover_tests(daichod_integration_tests)

# gtest_discover_tests' PROPERTIES ENVIRONMENT cannot carry two VAR=value
# entries in one value: it round-trips the string through
# cmake_parse_arguments() twice (once here, once inside the module's own
# discovery script), and each pass re-splits on any semicolon regardless of
# escaping, silently dropping every entry past the first. Set the two
# environment variables the fixture needs via a post-discovery include
# instead, using the per-test list gtest_discover_tests exports for exactly
# this purpose.
set(daichod_integration_tests_env_file
  ${CMAKE_CURRENT_BINARY_DIR}/daichod_integration_tests_env.cmake)
file(GENERATE OUTPUT ${daichod_integration_tests_env_file} CONTENT "\
foreach(_test IN LISTS daichod_integration_tests_TESTS)
  set_tests_properties(\"\${_test}\" PROPERTIES ENVIRONMENT
    \"DAICHOD_BIN=$<TARGET_FILE:daichod>;DAICHOD_MKBOOK=$<TARGET_FILE:daichod-mkbook>\")
endforeach()
")
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
  ${daichod_integration_tests_env_file})

# AccountService end-to-end tests: same daemon-over-socket approach, kept as
# a separate binary so it can grow independently of session_test.cpp. Needs
# daichod_proto (not just daichod_core) for the generated stubs plus
# google.rpc.Status for error-detail parsing.
add_executable(daichod_account_tests
  integration/account_test.cpp)
target_link_libraries(daichod_account_tests
  PRIVATE daichod_proto GTest::gtest_main)
add_dependencies(daichod_account_tests daichod daichod-mkbook)
gtest_discover_tests(daichod_account_tests)

set(daichod_account_tests_env_file
  ${CMAKE_CURRENT_BINARY_DIR}/daichod_account_tests_env.cmake)
file(GENERATE OUTPUT ${daichod_account_tests_env_file} CONTENT "\
foreach(_test IN LISTS daichod_account_tests_TESTS)
  set_tests_properties(\"\${_test}\" PROPERTIES ENVIRONMENT
    \"DAICHOD_BIN=$<TARGET_FILE:daichod>;DAICHOD_MKBOOK=$<TARGET_FILE:daichod-mkbook>\")
endforeach()
")
set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES
  ${daichod_account_tests_env_file})
