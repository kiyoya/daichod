find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(daichod_unit_tests
  unit/worker_test.cpp
  unit/error_test.cpp
  unit/map_test.cpp)
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
gtest_discover_tests(daichod_integration_tests
  PROPERTIES ENVIRONMENT
    "DAICHOD_BIN=$<TARGET_FILE:daichod>;DAICHOD_MKBOOK=$<TARGET_FILE:daichod-mkbook>")
