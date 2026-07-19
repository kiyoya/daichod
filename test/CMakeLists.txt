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

# Daemon-driving test binaries: each spawns the real daichod over its Unix
# socket against a fresh sqlite3 book, one binary per service area so they
# grow independently.
#
# The fixture needs DAICHOD_BIN/DAICHOD_MKBOOK. gtest_discover_tests'
# PROPERTIES ENVIRONMENT cannot carry two VAR=value entries in one value:
# it round-trips the string through cmake_parse_arguments() twice (once in
# the public macro, once inside the module's own discovery script), and
# each pass re-splits on any semicolon regardless of escaping, silently
# dropping every entry past the first. So the variables are set via a
# post-discovery include file instead, using the per-test list that
# gtest_discover_tests exports for exactly this purpose.
function(daichod_add_daemon_tests target source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE daichod_proto GTest::gtest_main)
  add_dependencies(${target} daichod daichod-mkbook)
  gtest_discover_tests(${target})
  set(env_file ${CMAKE_CURRENT_BINARY_DIR}/${target}_env.cmake)
  file(GENERATE OUTPUT ${env_file} CONTENT "\
foreach(_test IN LISTS ${target}_TESTS)
  set_tests_properties(\"\${_test}\" PROPERTIES ENVIRONMENT
    \"DAICHOD_BIN=$<TARGET_FILE:daichod>;DAICHOD_MKBOOK=$<TARGET_FILE:daichod-mkbook>\")
endforeach()
")
  set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES ${env_file})
endfunction()

daichod_add_daemon_tests(daichod_integration_tests integration/session_test.cpp)
daichod_add_daemon_tests(daichod_account_tests integration/account_test.cpp)
daichod_add_daemon_tests(daichod_transaction_tests
  integration/transaction_test.cpp)
# Crash-recovery kill matrix: DAICHOD_CRASH_AT at every protocol crash
# point, asserting the recovery handshake converges without loss or
# duplication.
daichod_add_daemon_tests(daichod_crash_tests integration/crash_test.cpp)
# Lifecycle: --backup-dir daily snapshot behavior across restarts.
daichod_add_daemon_tests(daichod_lifecycle_tests
  integration/lifecycle_test.cpp)
