/*
 * Copyright 2026 DynamicCanaryProject Authors
 *
 * 文件名: test_targets.c
 * 功能描述: 多攻击手法综合靶场程序。
 *
 *           集成三种经典栈漏洞场景，供多个攻击脚本复用：
 *
 *           靶场 1 —— vuln_standard()（Ret2Text / Ret2Libc / ROP）：
 *             32 字节缓冲区可被 256 字节覆盖；程序主动打印 system() 和
 *             sh_str 的运行时地址以规避 ASLR，便于专注于溢出攻防本身。
 *
 *           靶场 2 —— vuln_shellcode()（Ret2Shellcode）：
 *             64 字节缓冲区可被 256 字节覆盖；程序主动打印 buf 的栈地址
 *             供 shellcode 注入后精准跳转使用（需目标关闭 NX）。
 *
 *           靶场 3 —— vuln_off_by_one()（Off-By-One 栈帧迁移）：
 *             强制在 buf[n] 写入 '\0'，产生单字节越界，精确覆盖 saved RBP
 *             的最低位字节，触发栈帧错乱。
 *
 *           辅助符号：
 *             - backdoor()   : Ret2Text 的目标后门函数。
 *             - sh_str       : 全局 "/bin/sh" 字符串，供 Ret2Libc/ROP 使用。
 *             - rop_gadget() : 内嵌 `pop rdi; ret` 内联汇编，提供 ROP Gadget。
 *
 *           配套脚本: scripts/exp_ret2text.py, exp_ret2libc.py,
 *                     exp_rop.py, exp_shellcode.py, exp_offbyone.py
 * 作者: 陈文嘉
 * 创建日期: 2026-04-15
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// ================= 新增：高级攻击辅助弹药 =================
char *sh_str = "/bin/sh"; // 固定一个 /bin/sh 字符串地址

// 故意留下一个 pop rdi; ret 的 Gadget，用于构建 ROP 链
void rop_gadget() {
    __asm__("pop %rdi; ret");
}
// ==========================================================

// ---------------------------------------------------------
// 靶场后门：用于 Ret2Text 攻击
// ---------------------------------------------------------
void backdoor() {
    printf("\n[☠️] 警告：控制流已被劫持！成功跳入后门函数！\n");
    exit(0);
}

// ---------------------------------------------------------
// 漏洞 1：标准栈溢出 (可用于 Ret2Text, Ret2Libc, ROP)
// ---------------------------------------------------------
__attribute__((noinline)) void vuln_standard() {
    char buf[32]; // 触发我们的选择性插桩
    // 主动泄露地址，规避 ASLR干扰，以专注于溢出本身
    printf("[?] 靶场辅助：system 函数地址为 %p\n", &system);
    printf("[?] 靶场辅助：sh_str 变量地址为 %p\n", sh_str);
    printf("[?] 标准溢出靶场：请输入数据: ");
    // 严重溢出：读取 256 字节到 32 字节的缓冲区
    read(0, buf, 256); 
}

// ---------------------------------------------------------
// 漏洞 2：执行 Shellcode 专用靶场
// ---------------------------------------------------------
__attribute__((noinline)) void vuln_shellcode() {
    char buf[64];
    // 故意泄漏 buf 的栈地址，降低 ASLR 带来的干扰，让我们专注于 Canary 的攻防
    printf("[?] Shellcode 靶场：分配的栈缓冲区地址为 %p\n", buf);
    printf("[?] 请输入包含恶意机器码的 Shellcode: ");
    read(0, buf, 256);
}

// ---------------------------------------------------------
// 漏洞 3：Off-By-One 单字节溢出靶场
// ---------------------------------------------------------
__attribute__((noinline)) void vuln_off_by_one() {
    char buf[32];
    printf("[?] Off-By-One 靶场：请输入数据: ");
    // 只能输入 32 字节，看似安全...
    int n = read(0, buf, 32);
    // 致命的单字节溢出：强行在末尾补 '\0'，刚好覆盖老 RBP 的最低位字节！
    buf[n] = '\0'; 
}

int main(int argc, char **argv) {
    // 禁用缓冲区，确保 pwntools 交互顺畅
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2) {
        printf("========== 栈溢出攻防靶场 ==========\n");
        printf("用法: %s <靶场编号>\n", argv[0]);
        printf(" 1: 标准溢出 (对应 Ret2Text, Ret2Libc, ROP)\n");
        printf(" 2: 注入执行机器码 (对应 Ret2Shellcode)\n");
        printf(" 3: 单字节栈迁移 (对应 Off-By-One)\n");
        return 1;
    }

    int choice = atoi(argv[1]);
    switch (choice) {
        case 1: vuln_standard(); break;
        case 2: vuln_shellcode(); break;
        case 3: vuln_off_by_one(); break;
        default: printf("无效的靶场编号\n");
    }
    
    printf("[*] 程序正常执行完毕，即将退出。\n");
    return 0;
}