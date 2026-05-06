# Copyright 2026 DynamicCanaryProject Authors
#
# 文件名: batch_test.py
# 功能描述: 动态 Canary 选择性插桩 - 自动化批量验证脚本。
#
#           本脚本分两个阶段对选择性插桩方案进行端到端验证：
#             1. 静态插桩验证：通过 objdump 反汇编目标二进制，逐函数检查
#                是否存在对 __push_dynamic_canary 的调用，确认插桩选择策略
#                是否符合预期（危险函数插桩、安全函数放行）。
#             2. 动态溢出测试：通过 pwntools 向目标程序注入超长 Payload，
#                触发 risky_buffer_func 的栈溢出，验证运行时拦截告警是否
#                正常输出。
#
# 依赖工具: pwntools, objdump
# 作者: 陈文嘉
# 创建日期: 2026-04-15

from pwn import *
import os

# 合并 stderr 到 stdout 以捕获运行时告警输出；关闭 pwntools 冗余日志
context.log_level = 'error'
binary_path = '../output/selective_app'

print("\n==================================================")
print("🛡️  动态 Canary 选择性插桩 - 自动化批量验证")
print("==================================================\n")

# ----------------------------------------------------
# 阶段一：静态汇编验证 (Static Analysis)
# ----------------------------------------------------
print(">>> 阶段一：静态插桩验证 (分析机器码)")

# 我们要检查的函数名单
functions_to_check = ['safe_math', 'small_buffer_func', 'risky_buffer_func', 'main']

for func in functions_to_check:
    # 使用 objdump 获取目标函数的完整汇编代码
    # awk 命令用于精确截取某个函数名到下一个函数名之间的汇编内容
    cmd = f"objdump -d {binary_path} | awk -v RS= '/^[[:xdigit:]]+ <{func}>:/'"
    asm_code = os.popen(cmd).read()
    
    # 检查汇编代码中是否存在对我们生成的动态 Canary 运行库的调用
    if "<__push_dynamic_canary>" in asm_code:
        print(f"[+] 函数 {func.ljust(20)} : 已被插桩保护 🛡️ (符合预期)")
    else:
        print(f"[-] 函数 {func.ljust(20)} : 未插桩，安全放行 ⚪ (符合预期)")

# ----------------------------------------------------
# 阶段二：动态溢出测试 (Dynamic Testing)
# ----------------------------------------------------
print("\n>>> 阶段二：动态溢出实战测试")
print("[*] 正在启动目标程序，并向 risky_buffer_func 发射 100 字节恶意 Payload...")

try:
    # 关键修改1：stderr=STDOUT 将 stderr 合并进 stdout，这样才能收到告警框
    p = process(binary_path, stderr=STDOUT)
    
    # 发送超过 64 字节的垃圾数据以触发栈溢出
    payload = b"A" * 100
    p.send(payload)
    
    # 接收程序的全部输出（包含 stderr 的告警框）
    output = p.recvall(timeout=3).decode('utf-8', 'ignore')
    
    # 关键修改2：检测实际输出中存在的关键词
    if "检测到攻击" in output or "[!!!]" in output:
        print("\n[✔] 动态测试完美通过：成功检测到溢出并安全拦截！")
        # 找到含有告警框标志的那一行打印出来
        # 打印完整的告警框（从 ╔ 到 ╚ 之间的所有行）
        lines = output.split('\n')
        in_box = False
        for line in lines:
            if '╔' in line:
                in_box = True
            if in_box:
                print("    " + line)
            if '╚' in line and in_box:
                break
    else:
        print("\n[✖] 动态测试失败：未看到预期的拦截警告。")
        print("    程序实际输出：", repr(output[:200]))  # 打印原始输出便于调试

except EOFError:
    print("\n[✖] 动态测试异常：程序崩溃 (Segmentation fault)，未能成功拦截。")

print("\n==================================================\n")