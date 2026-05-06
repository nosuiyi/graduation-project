// Copyright 2026 DynamicCanaryProject Authors
//
// 文件名: canary_log.c
// 功能描述: 动态金丝雀异常日志模块的具体实现。
//
//           本模块在检测到栈溢出攻击时，执行以下操作：
//             1. 向 stderr 输出人类可读的告警信息（即时反馈）
//             2. 将结构化的事件记录追加写入日志文件（持久化存档）
//             3. 调用 abort() 终止进程并生成 core dump（便于取证）
//
//           日志文件路径优先级：
//             环境变量 CANARY_LOG_PATH > 默认路径 /tmp/canary_alert.log
//             (现已改为 “ /home/user/DynamicCanaryProject/canary_alert.log ”)
//
// 依赖模块: canary_log.h
// 作者: 陈文嘉
// 创建日期: 2026-04-15

#include "canary_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>  // 用于 syscall(SYS_gettid) 获取真实线程 ID

// ============================================================
// 模块内部状态
// ============================================================

// 日志文件的实际写入路径。
// 在 __init_canary_log() 中确定，之后只读。
static char g_log_path[256] = CANARY_DEFAULT_LOG_PATH;

// ============================================================
// 内部辅助函数
// ============================================================

// 将 CanaryEventType 枚举值转换为可读字符串，用于日志格式化。
//
// 参数:
//   event_type - 事件类型枚举值
//
// 返回值:
//   指向静态字符串的指针，调用方不需要也不应该 free() 该指针
static const char *EventTypeToString(CanaryEventType event_type) {
    switch (event_type) {
        case CANARY_EVENT_STACK_OVERFLOW:   return "STACK_OVERFLOW";
        case CANARY_EVENT_SHADOW_OVERFLOW:  return "SHADOW_STACK_OVERFLOW";
        case CANARY_EVENT_SHADOW_UNDERFLOW: return "SHADOW_STACK_UNDERFLOW";
        default:                            return "UNKNOWN_EVENT";
    }
}

// 获取当前时间的格式化字符串，写入调用方提供的缓冲区。
//
// 输出格式示例: "2026-04-15 10:23:47"
//
// 参数:
//   buf     - 输出缓冲区，由调用方分配
//   buf_len - 缓冲区长度，建议不小于 32 字节
static void GetCurrentTimeString(char *buf, size_t buf_len) {
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    if (tm_info != NULL) {
        strftime(buf, buf_len, "%Y-%m-%d %H:%M:%S", tm_info);
    } else {
        // localtime 极少数情况下可能失败，提供兜底值
        strncpy(buf, "unknown-time", buf_len - 1);
        buf[buf_len - 1] = '\0';
    }
}

// 将一条完整的告警事件追加写入日志文件。
//
// 日志条目格式（示例）：
//   [2026-04-15 10:23:47] EVENT=STACK_OVERFLOW TID=12345
//     FUNC_ID=0xabcdef1234567890
//     STORED_CANARY=0xdeadbeefcafebabe
//     EXPECTED_CANARY=0xffc9493eade7e0e3
//     SHADOW_DEPTH=3
//
// 参数:
//   info - 指向已填充好的告警信息结构体的指针
static void WriteAlertToFile(const CanaryAlertInfo *info) {
    // 以追加模式打开日志文件。
    // 即使多个进程/线程同时写入，追加模式在 Linux 下对单次 write() 是原子的。
    FILE *log_file = fopen(g_log_path, "a");
    if (log_file == NULL) {
        // 文件打开失败时仅向 stderr 输出警告，不影响主告警流程
        fprintf(stderr, "[Canary Log] 警告：无法写入日志文件 '%s'\n", g_log_path);
        return;
    }

    char time_str[32];
    GetCurrentTimeString(time_str, sizeof(time_str));

    fprintf(log_file,
            "[%s] EVENT=%-22s TID=%-8d\n"
            "  FUNC_ID         = 0x%016lx\n"
            "  STORED_CANARY   = 0x%016lx\n"
            "  EXPECTED_CANARY = 0x%016lx\n"
            "  SHADOW_DEPTH    = %d\n"
            "\n",
            time_str,
            EventTypeToString(info->event_type),
            (int)info->tid,
            (unsigned long)info->func_id,
            (unsigned long)info->stored_canary,
            (unsigned long)info->expected_canary,
            info->shadow_depth);

    fclose(log_file);
}

// ============================================================
// 公共接口实现
// ============================================================

// 模块初始化：确定日志路径，写入会话起始标记。
//
// 使用 constructor 优先级 101（高于 canary_rt.c 中默认优先级的 constructor），
// 确保日志模块在运行时核心初始化之前就绪。
__attribute__((constructor(101))) void __init_canary_log(void) {
    // 检查是否有环境变量覆盖默认日志路径
    const char *env_path = getenv("CANARY_LOG_PATH");
    if (env_path != NULL && strlen(env_path) > 0) {
        strncpy(g_log_path, env_path, sizeof(g_log_path) - 1);
        g_log_path[sizeof(g_log_path) - 1] = '\0';
    }

    // 写入本次运行的会话头，便于在日志文件中区分不同的程序运行实例
    FILE *log_file = fopen(g_log_path, "a");
    if (log_file == NULL) {
        // 初始化阶段无法写入日志文件，向 stderr 报告后继续运行（非致命错误）
        fprintf(stderr, "[Canary Log] 警告：日志文件 '%s' 不可写，告警将仅输出到 stderr\n",
                g_log_path);
        return;
    }

    char time_str[32];
    GetCurrentTimeString(time_str, sizeof(time_str));
    fprintf(log_file,
            "=== Canary Session Start | PID: %-6d | %s ===\n",
            (int)getpid(),
            time_str);
    fclose(log_file);
}

// 触发安全告警的统一入口，详见头文件中的接口说明。
__attribute__((noreturn)) void __canary_alert(const CanaryAlertInfo *info) {
    char time_str[32];
    GetCurrentTimeString(time_str, sizeof(time_str));

    // --- 第一步：向 stderr 输出即时可见的人类友好告警 ---
    fprintf(stderr,
            "\n"
            "╔══════════════════════════════════════════════════════╗\n"
            "║           [!!!] 动态金丝雀：检测到攻击！             ║\n"
            "╠══════════════════════════════════════════════════════╣\n"
            "║  事件类型  : %-40s║\n"
            "║  发生时间  : %-40s║\n"
            "║  线程 ID   : %-40d║\n"
            "║  函数 ID   : 0x%-38lx║\n"
            "║  栈帧 Canary: 0x%-36lx║\n"
            "║  预期 Canary: 0x%-36lx║\n"
            "║  影子栈深度: %-40d║\n"
            "║  日志路径  : %-40s║\n"
            "╚══════════════════════════════════════════════════════╝\n",
            EventTypeToString(info->event_type),
            time_str,
            (int)info->tid,
            (unsigned long)info->func_id,
            (unsigned long)info->stored_canary,
            (unsigned long)info->expected_canary,
            info->shadow_depth,
            g_log_path);

    // --- 第二步：将结构化事件写入日志文件 ---
    WriteAlertToFile(info);

    // --- 第三步：调用 abort() 终止进程 ---
    // 使用 abort() 而非 exit()，原因：
    //   1. abort() 会触发 SIGABRT 信号，操作系统将生成 core dump 文件
    //   2. core dump 保留了完整的栈帧和内存状态，便于取证分析攻击细节
    //   3. exit() 会正常清理资源，可能破坏现场证据
    abort();
}
