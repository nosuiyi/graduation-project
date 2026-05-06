# Copyright 2026 DynamicCanaryProject Authors
#
# 文件名: CMakeLists.txt
# 功能描述: DynamicCanaryPass 项目构建配置。
#
#           管理两个构建目标：
#             1. DynamicCanary（共享库 .so）
#                LLVM 插桩 Pass 插件，由 pass/DynamicCanary.cpp 编译而来。
#                使用 -fno-rtti 以与 LLVM 默认编译选项保持一致（LLVM 本身
#                关闭了 RTTI，混用会导致链接时符号不兼容）。
#                使用方式：opt --load-pass-plugin=build/libDynamicCanary.so
#                          clang -fpass-plugin=build/libDynamicCanary.so
#
#             2. canary_rt（静态库 .a）
#                动态 Canary 运行时支持库，由 runtime/canary_rt.c 编译而来。
#                提供 __push_dynamic_canary() 和 __check_dynamic_canary()
#                两个插桩调用点的实现，链接至被保护的目标程序。
#
# 构建方式:
#   mkdir -p build && cd build
#   cmake .. && make
#
# 作者: 陈文嘉
# 创建日期: 2026-04-15

cmake_minimum_required(VERSION 3.10)
project(DynamicCanaryPass)

# 定位系统中已安装的 LLVM，获取头文件路径、编译宏定义等配置
find_package(LLVM REQUIRED CONFIG)
add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})

# 目标一：编译 LLVM Pass 插件（动态共享库 .so）
# -fno-rtti：与 LLVM 保持 RTTI 设置一致，避免链接时 ABI 不兼容
add_library(DynamicCanary SHARED pass/DynamicCanary.cpp)
set_target_properties(DynamicCanary PROPERTIES COMPILE_FLAGS "-fno-rtti")

# 目标二：编译动态 Canary 运行时静态库（.a）
# 被保护的目标程序在链接阶段需要链接此库
add_library(canary_rt STATIC runtime/canary_rt.c)