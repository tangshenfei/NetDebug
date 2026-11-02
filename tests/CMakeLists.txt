add_executable(test_core
    test_main.c
    test_util.c
    test_container.c
    test_addr.c
    test_url.c
    test_planner.c
    test_http.c
    test_telnet.c
    ${WEB_ASSETS_C}
)
add_dependencies(test_core web_assets)

target_include_directories(test_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_core PRIVATE ndlib)

add_test(NAME core COMMAND test_core)

# 生成 JUnit XML + Markdown 报告，便于 CI 收集结果
add_test(NAME core_report
         COMMAND test_core --junit ${CMAKE_BINARY_DIR}/test-results.xml
                          --md ${CMAKE_BINARY_DIR}/test-report.md)
