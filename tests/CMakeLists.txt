find_package(GTest REQUIRED)

add_executable(tests
    unit_tests.cpp
    integration_tests.cpp
)
target_link_libraries(tests PRIVATE hffix GTest::gtest GTest::gtest_main)
target_compile_definitions(tests PRIVATE
    HFFIX_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

include(GoogleTest)
gtest_discover_tests(tests)
