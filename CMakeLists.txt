# 智慧农场 ESP32 主程序 —— 工程根 CMakeLists
#
# 以下顺序是 ESP-IDF 的硬性要求，不可调整：
#   1. cmake_minimum_required
#   2. include($ENV{IDF_PATH}/tools/cmake/project.cmake)
#   3. project()
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 工程名（ASCII，与磁盘上的中文目录名解耦，避免构建系统对非 ASCII 路径的兼容问题）
project(smartfarm)
