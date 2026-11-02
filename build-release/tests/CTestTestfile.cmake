# CMake generated Testfile for 
# Source directory: C:/Users/tsf12/Documents/deepseek-harness/default-workspace/tests
# Build directory: C:/Users/tsf12/Documents/deepseek-harness/default-workspace/build-release/tests
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test("core" "C:/Users/tsf12/Documents/deepseek-harness/default-workspace/build-release/tests/test_core.exe")
set_tests_properties("core" PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/tsf12/Documents/deepseek-harness/default-workspace/tests/CMakeLists.txt;17;add_test;C:/Users/tsf12/Documents/deepseek-harness/default-workspace/tests/CMakeLists.txt;0;")
add_test("core_report" "C:/Users/tsf12/Documents/deepseek-harness/default-workspace/build-release/tests/test_core.exe" "--junit" "C:/Users/tsf12/Documents/deepseek-harness/default-workspace/build-release/test-results.xml" "--md" "C:/Users/tsf12/Documents/deepseek-harness/default-workspace/build-release/test-report.md")
set_tests_properties("core_report" PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/tsf12/Documents/deepseek-harness/default-workspace/tests/CMakeLists.txt;20;add_test;C:/Users/tsf12/Documents/deepseek-harness/default-workspace/tests/CMakeLists.txt;0;")
