# nlohmann_json - JSON for Modern C++ (Header-only)
cmake_minimum_required(VERSION 3.20)

# 创建纯头文件库接口
add_library(nlohmann_json INTERFACE)

# 添加包含目录（头文件直接在根目录）
target_include_directories(nlohmann_json INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 别名，兼容 find_package 方式
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
