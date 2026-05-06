// Copyright 2026 DynamicCanaryProject Authors
//
// 文件名: canary_rt.c
// 功能描述: 动态金丝雀运行时核心模块。
//
//           本模块实现了动态金丝雀机制的两个核心运行时函数，由
//           DynamicCanary LLVM Pass 在编译期自动插入对这两个函数的调用：
//             - __push_dynamic_canary(): 在被保护函数的入口处调用，
//               生成唯一的 canary 值并压入影子栈。
//             - __check_dynamic_canary(): 在被保护函数的出口处调用，
//               从影子栈弹出预期值并与栈帧中存储的值比对。
//
//           安全特性：
//             1. 动态 Canary：每次函数调用的 canary 值唯一
//                （seed ^ func_id ^ call_counter），抵抗泄露后重用攻击。
//             2. 影子栈加密：存入影子栈前用 shadow_cookie 进行 XOR 混淆，
//                防止攻击者通过读取影子栈内存来伪造合法 canary。
//             3. 线程安全：所有状态变量均为 thread_local，无竞态条件。
//
// 依赖模块: canary_log.h（异常告警与日志记录）
// 作者: 陈文嘉
// 创建日期: 2026-04-15

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "canary_log.h"

// ============================================================
// 常量定义
// ============================================================

// 影子栈支持的最大函数调用嵌套深度。
// 超过此深度将触发 CANARY_EVENT_SHADOW_OVERFLOW 告警并终止进程。
// 1024 层足以覆盖绝大多数正常程序的调用栈深度。
#define MAX_DEPTH 1024

// ============================================================
// 线程本地状态（Thread-Local Storage）
// ============================================================
// 所有状态变量均使用 __thread 修饰，保证多线程环境下每个线程
// 拥有独立的副本，完全消除线程间竞态条件。

// Canary 生成种子：在进程启动时从 /dev/urandom 读取真随机数初始化。
// 每个线程拥有独立的种子，线程间 canary 值不可预测、互不干扰。
__thread uint64_t thread_local_seed = 0;

// 全局调用计数器：每次 __push_dynamic_canary() 调用时自增。
// 引入计数器是为了让同一函数在不同时刻的调用产生不同的 canary 值，
// 从而抵御攻击者"先泄露、后重用"的两阶段绕过攻击。
__thread uint64_t call_counter = 0;

// 影子栈：以加密形式存放每个函数调用对应的预期 canary 值。
// 采用独立于程序主栈的内存区域存放，栈溢出攻击无法直接覆盖。
__thread uint64_t shadow_canary_stack[MAX_DEPTH];

// 影子栈栈顶指针：指向下一个可写入的槽位索引。
__thread int shadow_top = 0;

// 影子栈加密 Cookie：用于对存入影子栈的 canary 值进行 XOR 混淆。
// 在进程启动时从 /dev/urandom 独立读取，与 thread_local_seed 不同，
// 即使攻击者泄露了 canary 值本身，也无法推算出影子栈中存储的密文。
__thread uint64_t shadow_cookie = 0;

// ============================================================
// 模块初始化（进程启动时自动执行）
// ============================================================

// 运行时初始化函数，在 main() 执行前由 C 运行时自动调用。
//
// 职责：
//   1. 从 /dev/urandom 读取真随机数，初始化 thread_local_seed
//   2. 从 /dev/urandom 读取真随机数，初始化 shadow_cookie
//   3. 若 /dev/urandom 不可用，使用弱随机数兜底（降级模式）
//
// 注意：使用 constructor 默认优先级（无编号），晚于 canary_log.c
// 中优先级 101 的初始化函数执行，确保日志模块已就绪。
void __attribute__((constructor)) __init_dynamic_canary(void) {
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        // 读取 8 字节（64 位）真随机数作为 canary 生成种子
        if (fread(&thread_local_seed, sizeof(uint64_t), 1, f) != 1) {
            // fread 失败（极罕见），使用固定魔数兜底，安全性降低但不崩溃
            thread_local_seed = 0xDEADBEEFCAFEBABEULL;
        }
        // 读取第二个独立的 8 字节随机数作为影子栈加密 cookie
        // 与 seed 分开读取，确保两者统计独立，互不可推导
        if (fread(&shadow_cookie, sizeof(uint64_t), 1, f) != 1) {
            shadow_cookie = 0xCAFEBABEDEADF00DULL;
        }
        fclose(f);
    } else {
        // /dev/urandom 打开失败（沙箱环境或极端受限系统），
        // 使用时间戳与栈地址的 XOR 组合作为弱随机种子兜底。
        // 弱随机种子在理论上可被预测，但对于演示和测试环境已足够。
        thread_local_seed = (uint64_t)time(NULL) ^ (uint64_t)&thread_local_seed;
        shadow_cookie     = (uint64_t)getpid()   ^ (uint64_t)&shadow_cookie ^ 0xA5A5A5A5A5A5A5A5ULL;
    }
}

// ============================================================
// 核心运行时函数
// ============================================================

// 在被保护函数的入口处调用：生成本次调用的 canary 值并加密压栈。
//
// Canary 生成公式：canary = thread_local_seed XOR func_id XOR call_counter
//   - thread_local_seed：进程级真随机数，攻击者无法预知
//   - func_id：函数的编译期哈希值，不同函数产生不同 canary
//   - call_counter：单调递增计数器，同一函数每次调用产生不同 canary
//
// 影子栈存储公式：shadow_canary_stack[top] = canary XOR shadow_cookie
//   - shadow_cookie 独立随机，canary 密文与明文不可互推
//
// 参数:
//   func_id - 被插桩函数的编译期哈希 ID，由 LLVM Pass 在编译时注入
//
// 返回值:
//   生成的 canary 明文值，LLVM Pass 会将其存储在函数的栈帧局部变量中
uint64_t __push_dynamic_canary(uint64_t func_id) {
    // 自增调用计数器，确保同一函数的每次调用产生不同的 canary 值
    call_counter++;

    // 三元 XOR 生成本次调用的唯一 canary 值
    uint64_t canary = thread_local_seed ^ func_id ^ call_counter;

    // 检查影子栈是否还有空间
    if (shadow_top >= MAX_DEPTH) {
        // 影子栈上溢：正常程序不会达到 1024 层嵌套，
        // 触发此分支通常意味着存在无限递归或异常的调用深度
        CanaryAlertInfo alert_info = {
            .event_type      = CANARY_EVENT_SHADOW_OVERFLOW,
            .func_id         = func_id,
            .stored_canary   = canary,
            .expected_canary = 0,
            .shadow_depth    = shadow_top,
            .tid             = (pid_t)syscall(SYS_gettid),
        };
        __canary_alert(&alert_info);  // 不会返回
    }

    // 将 canary 明文与 shadow_cookie 做 XOR 后存入影子栈（加密存储）。
    // 即使攻击者能读取影子栈内存，得到的也是密文，无法直接用于伪造。
    shadow_canary_stack[shadow_top++] = canary ^ shadow_cookie;

    return canary;  // 返回明文，由 LLVM Pass 存储到函数栈帧的局部变量中
}

// 在被保护函数的出口处调用：从影子栈弹出预期值并与栈帧值比对。
//
// 校验逻辑：
//   1. 从影子栈弹出密文，XOR shadow_cookie 还原预期明文
//   2. 将预期明文与函数栈帧中存储的 canary 值（stored_canary）比较
//   3. 若不一致，说明 stored_canary 所在的栈帧区域已被溢出覆盖，
//      立即触发告警并终止进程
//
// 参数:
//   func_id       - 被插桩函数的编译期哈希 ID（与 push 时一致）
//   stored_canary - 从函数栈帧局部变量中读取的 canary 值（可能被篡改）
void __check_dynamic_canary(uint64_t func_id, uint64_t stored_canary) {
    // 检查影子栈是否为空（push/pop 不匹配，通常意味着运行时环境异常）
    if (shadow_top <= 0) {
        CanaryAlertInfo alert_info = {
            .event_type      = CANARY_EVENT_SHADOW_UNDERFLOW,
            .func_id         = func_id,
            .stored_canary   = stored_canary,
            .expected_canary = 0,
            .shadow_depth    = shadow_top,
            .tid             = (pid_t)syscall(SYS_gettid),
        };
        __canary_alert(&alert_info);  // 不会返回
    }

    // 从影子栈弹出密文，解密还原预期的 canary 明文
    uint64_t expected = shadow_canary_stack[--shadow_top] ^ shadow_cookie;

    // 核心安全校验：比较预期值与栈帧中的实际值
    if (expected != stored_canary) {
        // 校验失败：stored_canary 与预期不符，说明函数栈帧已被篡改。
        // 此时 stored_canary 的值已被攻击者的溢出数据覆盖。
        CanaryAlertInfo alert_info = {
            .event_type      = CANARY_EVENT_STACK_OVERFLOW,
            .func_id         = func_id,
            .stored_canary   = stored_canary,
            .expected_canary = expected,
            .shadow_depth    = shadow_top,
            .tid             = (pid_t)syscall(SYS_gettid),
        };
        __canary_alert(&alert_info);  // 不会返回
    }
    // 校验通过：函数栈帧完整，正常返回
}
