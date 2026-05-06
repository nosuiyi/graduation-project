/*
 * Copyright 2026 DynamicCanaryProject Authors
 *
 * 文件名: test_vuln.c
 * 功能描述: 基础 Ret2Text 攻击对比验证靶场程序。
 *
 *           提供最简化的溢出场景，专门用于对比无保护版本（vuln_app）
 *           和动态 Canary 保护版本（protected_app）在面对 Ret2Text
 *           攻击时的行为差异：
 *
 *           - hacked()         : 后门函数，控制流一旦跳入此处即宣告攻击成功。
 *           - vulnerable_func(): 8 字节缓冲区允许读取 64 字节，存在典型
 *                                 栈溢出漏洞；函数内含 buffer[8] 数组，会
 *                                 触发选择性插桩保护。
 *
 *           预期结果：
 *             - vuln_app      : 攻击成功，控制流跳入 hacked()。
 *             - protected_app : Canary 校验失败，攻击被拦截，告警触发。
 *
 *           配套脚本: scripts/exp.py
 * 作者: 陈文嘉
 * 创建日期: 2026-04-15
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

void hacked() {
    // 使用 puts 可以避免 64位 Ubuntu 下 printf 的栈对齐崩溃问题
    puts("\n>>> 🚨 警告：控制流已被劫持！已成功跳入后门函数！🚨 <<<");
    exit(0);
}

void vulnerable_func() {
    char buffer[8];
    printf("请输入内容: ");
    fflush(stdout); // 刷新缓冲区，确保提示语立刻显示在终端
    
    // 典型漏洞点：buffer 只有 8 字节，但我允许你输入 64 字节！
    // read(文件描述符0代表标准输入, 存入buffer, 最大读取字节数)
    read(0, buffer, 64); 
    
    printf("输入的内容已接收。\n");
}

int main() {
    vulnerable_func();
    return 0;
}