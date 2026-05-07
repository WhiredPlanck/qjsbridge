cmake_minimum_required(VERSION 3.16)

# Minimal test runner using CTest (no external framework required)
function(add_qjsb_test target src)
    add_executable(${target} ${src})
    target_link_libraries(${target} PRIVATE qjsbridge_with_quickjs)
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

add_qjsb_test(test_core    test_core.cpp)
add_qjsb_test(test_convert test_convert.cpp)
add_qjsb_test(test_function test_function.cpp)
add_qjsb_test(test_class   test_class.cpp)
