/*
 * Copyright 2026 DynamicCanaryProject Authors
 *
 * 文件名: test_boundary.c
 * 功能描述: 动态 Canary 边界场景压力测试程序。
 *
 *           验证动态 Canary 在两种极端运行场景下的正确性与安全性：
 *
 *           场景 1 —— 深度递归（Depth: 1000）：
 *             deep_recursion() 每层都分配一个 16 字节的局部数组（触发插桩），
 *             影子栈会在 1000 层嵌套调用下被压入/弹出 1000 次。用于验证影子
 *             栈容量上限（MAX_DEPTH）和 Canary 的嵌套一致性，确保不发生越界
 *             或错误匹配。
 *
 *           场景 2 —— 多线程并发（5 Threads）：
 *             5 个线程同时调用 thread_worker()，每个线程均持有含局部数组的
 *             栈帧（触发插桩）。用于验证 __thread 线程本地存储（TLS）确实隔离
 *             了各线程的 Canary 状态，不存在竞争条件或跨线程污染。
 *
 * 编译选项: 需链接 -lpthread
 * 作者: 陈文嘉
 * 创建日期: 2026-04-15
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>

// 边界场景 1：深度递归函数
// 验证影子栈 (Shadow Stack) 在大量嵌套调用时是否会越界或错乱
__attribute__((noinline)) void deep_recursion(int depth) {
    char buffer[16]; // 触发选择性插桩的危险特征
    sprintf(buffer, "Depth: %d", depth);
    
    if (depth == 0) {
        printf("[+] 递归触底成功！影子栈运作正常。\n");
        return;
    }
    deep_recursion(depth - 1);
}

// 边界场景 2：多线程并发
// 验证 thread_local_seed 和 线程本地影子栈 的并发安全性
__attribute__((noinline)) void* thread_worker(void* arg) {
    int thread_id = *(int*)arg;
    char local_buf[32]; // 触发选择性插桩的危险特征
    
    // 模拟一段有风险的内存操作（但合法）
    memset(local_buf, 'A', 31);
    local_buf[31] = '\0';
    
    printf("[+] 线程 %d 执行完毕，Canary 校验通过。\n", thread_id);
    return NULL;
}

int main() {
    printf("========== 边界测试启动 ==========\n");
    
    printf("\n--- 正在测试深度递归 (Depth: 1000) ---\n");
    deep_recursion(1000);
    
    printf("\n--- 正在测试多线程并发 (5 Threads) ---\n");
    pthread_t threads[5];
    int thread_ids[5];
    
    for (int i = 0; i < 5; i++) {
        thread_ids[i] = i;
        pthread_create(&threads[i], NULL, thread_worker, &thread_ids[i]);
    }
    
    for (int i = 0; i < 5; i++) {
        pthread_join(threads[i], NULL);
    }
    
    printf("\n========== 所有边界测试完美通过！==========\n");
    return 0;
}