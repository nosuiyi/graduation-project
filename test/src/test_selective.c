/*
 * Copyright 2026 DynamicCanaryProject Authors
 *
 * 文件名: test_selective.c
 * 功能描述: 动态 Canary 选择性插桩策略验证靶场程序。
 *
 *           包含四个精心设计的函数，覆盖插桩策略的核心判断分支：
 *
 *           函数                   风险特征              预期结果
 *           ──────────────────────────────────────────────────────
 *           safe_math()          无局部数组              不插桩（放行）
 *           small_buffer_func()  4 字节数组（< 8 字节）  不插桩（低风险放行）
 *           risky_buffer_func()  64 字节数组（≥ 8 字节） 插桩保护
 *           main()               无局部数组              不插桩（放行）
 *
 *           risky_buffer_func() 故意允许读入 128 字节到 64 字节的缓冲区，
 *           制造真实的栈溢出漏洞，供 batch_test.py 的动态测试阶段触发。
 *
 *           配套脚本: scripts/batch_test.py
 * 作者: 陈文嘉
 * 创建日期: 2026-04-15
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

// 1. 安全函数：完全没有局部数组
int safe_math(int a, int b) {
    int sum = a + b;
    int diff = a - b;
    return sum * diff;
}

// 2. 低风险函数：缓冲区较小 (4 字节，小于我们设定的 8 字节阈值)
void small_buffer_func() {
    char flag[4];
    strcpy(flag, "OK");
}

// 3. 高风险函数：存在大容量缓冲区，且使用 read 接收外部输入
void risky_buffer_func() {
    char buffer[64];
    // 允许读取 128 字节，故意制造溢出漏洞
    read(0, buffer, 128); 
}

int main() {
    safe_math(10, 5);
    small_buffer_func();
    risky_buffer_func();
    return 0;
}