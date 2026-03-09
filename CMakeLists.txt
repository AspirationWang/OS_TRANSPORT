cmake_minimum_required(VERSION 3.10)
project(OS_TRANSPORT VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
# 关闭隐式函数声明警告
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-implicit-function-declaration")

# 收集所有源码
file(GLOB SRC_FILES src/*.c)

# 编译动态库
add_library(os_transport SHARED
    ${SRC_FILES}
)

# 链接pthread（线程依赖）
target_link_libraries(os_transport pthread)

target_include_directories(os_transport
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(os_transport PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME "os_transport"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# 安装规则（包含线程池头文件）
install(
    TARGETS os_transport
    LIBRARY DESTINATION lib64
    COMPONENT runtime
)

install(
    FILES include/os_transport.h
          include/os_transport_thread_pool.h
    DESTINATION include/os-transport
    COMPONENT devel
)

# 编译测试程序
add_executable(test_thread_pool test/test_thread_pool.c)
target_link_libraries(test_thread_pool os_transport pthread)