// Copyright 2025 DynamicCanaryProject Authors
//
// 文件名: canary_log.h
// 功能描述: 动态金丝雀异常日志模块的公共接口声明。
//           提供结构化的安全事件记录能力，供运行时核心模块调用。
// 作者: 陈文嘉
// 创建日期: 2026-04-15

#ifndef CANARY_LOG_H_
#define CANARY_LOG_H_

#include <stdint.h>
#include <sys/types.h>

// 日志文件的默认写入路径。
// 运行时如果环境变量 CANARY_LOG_PATH 存在，则优先使用该变量指定的路径。
#define CANARY_DEFAULT_LOG_PATH "/home/user/DynamicCanaryProject/canary_alert.log"

// 异常事件的类型枚举。
// 用于区分不同来源的安全告警，便于日志分类与后续分析。
typedef enum {
    CANARY_EVENT_STACK_OVERFLOW   = 1,  // 标准栈溢出：canary 校验失败
    CANARY_EVENT_SHADOW_OVERFLOW  = 2,  // 影子栈上溢：调用深度超过 MAX_DEPTH
    CANARY_EVENT_SHADOW_UNDERFLOW = 3,  // 影子栈下溢：不匹配的 push/pop 调用
} CanaryEventType;

// 一条安全告警事件的完整描述结构体。
// 所有字段在 __canary_alert() 调用时由运行时自动填充。
typedef struct {
    CanaryEventType event_type;    // 事件类型（见上方枚举）
    uint64_t        func_id;       // 发生异常的函数哈希 ID（由 Pass 在编译期注入）
    uint64_t        stored_canary; // 从函数栈帧中读取的 canary 值（可能已被篡改）
    uint64_t        expected_canary; // 影子栈中存储的预期 canary 值
    int             shadow_depth;  // 异常发生时影子栈的当前深度
    pid_t           tid;           // 触发异常的线程 ID
} CanaryAlertInfo;

// 触发安全告警的统一入口函数。
//
// 该函数负责：
//   1. 将告警信息格式化并输出到 stderr（即时可见）
//   2. 将结构化日志追加写入到日志文件（持久化存档）
//   3. 调用 abort() 终止进程（产生 core dump，便于事后调试）
//
// 参数:
//   info - 指向已填充好的告警信息结构体的指针，不能为 NULL
//
// 注意: 该函数不会返回（标记为 noreturn）。
__attribute__((noreturn)) void __canary_alert(const CanaryAlertInfo *info);

// 模块初始化函数：确定日志文件路径并写入会话头信息。
//
// 该函数由 __attribute__((constructor)) 标记，在 main() 执行前自动调用。
// 写入内容示例：
//   === Canary Session Start | PID: 12345 | 2025-04-15 10:00:00 ===
void __init_canary_log(void);

#endif  // CANARY_LOG_H_
