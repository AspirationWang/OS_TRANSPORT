# 指定CMake最低版本（兼容主流系统）
cmake_minimum_required(VERSION 3.10)

# 项目名称、语言（C）、版本
project(OS_TRANSPORT VERSION 1.0 LANGUAGES C)

# 设置C语言标准（C99，兼容大部分编译器）
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 编译选项：开启警告、优化等级O2，生成位置无关代码（共享库必需）
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2 -fPIC")

# 定义共享库目标（最终生成libos_transport.so）
add_library(os_transport SHARED
    src/os_transport.c
)

# 指定头文件搜索路径（对外暴露的头文件）
target_include_directories(os_transport
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include  # 外部使用时可找到头文件
)

# 设置共享库属性（版本、输出名）
set_target_properties(os_transport PROPERTIES
    VERSION ${PROJECT_VERSION}          # 版本号（如1.0）
    SOVERSION ${PROJECT_VERSION_MAJOR}  # 主版本号（如1）
    OUTPUT_NAME "os_transport"          # 强制指定库名
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}  # 库输出到build目录
)