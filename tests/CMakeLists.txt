find_package(GTest REQUIRED)

add_executable(parameter_test
        parameter.unittest.cxx
)
target_link_libraries(parameter_test PRIVATE parameter GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(parameter_test)
