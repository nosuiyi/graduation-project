/*
 * Copyright 2026 DynamicCanaryProject Authors
 *
 * 文件名: test_bypass.c
 * 功能描述: Canary 绕过攻防对比靶场程序。
 *
 *           模拟一个典型的双漏洞应用场景，将"信息泄露"和"控制流劫持"
 *           组合为两步攻击链，用于对比静态 Canary 与动态 Canary 的抗绕过能力：
 *
 *           漏洞一 —— 格式化字符串（echo_service）：
 *             将用户输入直接传给 printf()，攻击者可通过 %N$p 格式化占位符
 *             读取栈上任意偏移处的值，实现 Canary 值泄露。
 *
 *           漏洞二 —— 栈溢出（vuln_service）：
 *             32 字节缓冲区允许读取 256 字节，攻击者将泄露的 Canary 原样
 *             写回，企图绕过校验并覆盖返回地址跳入 backdoor()。
 *
 *           预期防御效果：
 *             - 基线版（静态 Canary）: Canary 固定，泄露后即可成功绕过。
 *             - 保护版（动态 Canary）: 每次调用生成不同 Canary，且与函数 ID
 *               绑定，泄露的旧值无法通过校验，攻击被拦截。
 *
 *           配套脚本: scripts/exp_bypass.py
 * 作者: 陈文嘉
 * 创建日期: 2026-04-15
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

void backdoor() {
    printf("\n[☠️] 致命打击：原生静态 Canary 已被绕过！成功夺取系统控制权！\n");
    exit(0);
}

// 漏洞功能 1：格式化字符串漏洞（用于泄露 Canary）
__attribute__((noinline)) void echo_service() {
    char buf[64];
    printf("[?] (Echo服务) 请输入要回显的内容: ");
    read(0, buf, 63);
    printf("系统回显: ");
    // 致命漏洞：直接打印用户输入，导致格式化字符串泄露
    printf(buf); 
    printf("\n");
}

// 漏洞功能 2：标准栈溢出漏洞（用于劫持控制流）
__attribute__((noinline)) void vuln_service() {
    char buf[32];
    printf("[?] (Vuln服务) 请输入数据: ");
    // 致命漏洞：读取 256 字节，导致严重的栈溢出
    read(0, buf, 256); 
}

int main() {
    // 禁用缓冲区，保证 pwntools 通信顺畅
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    char choice_buf[16];
    int choice;

    printf("========== Canary 绕过终极对抗靶场 ==========\n");
    while(1) {
        printf("\n1. Echo 留言服务 (存在泄露漏洞)\n");
        printf("2. Vuln 数据处理 (存在溢出漏洞)\n");
        printf("3. 退出\n");
        printf("请选择: ");
        
        read(0, choice_buf, 15);
        choice = atoi(choice_buf);

        if (choice == 1) {
            echo_service();
        } else if (choice == 2) {
            vuln_service();
        } else {
            break;
        }
    }
    return 0;
}