cmake_minimum_required(VERSION 3.16)

function(add_qjsb_example target src)
    add_executable(${target} ${src})
    target_link_libraries(${target} PRIVATE qjsbridge_with_quickjs)
endfunction()

add_qjsb_example(example_hello      hello.cpp)
add_qjsb_example(example_class      class_binding.cpp)
add_qjsb_example(example_containers containers.cpp)
add_qjsb_example(example_module     module_binding.cpp)
add_qjsb_example(example_custom_module_loader custom_module_loader.cpp)
