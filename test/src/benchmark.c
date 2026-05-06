/*
 * Copyright 2026 DynamicCanaryProject Authors
 *
 * 文件名: benchmark.c
 * 功能描述: 动态 Canary 防御方案性能基准测试目标程序。
 *
 *           提供四类函数供 perf_test.py 进行基线 vs 保护版本的对比测量：
 *             - safe_math_compute  : 纯算术运算，无数组，不应被插桩。
 *             - risky_light        : 含 64 字节局部数组 + 64 次计算（轻量函数体）。
 *             - risky_medium       : 含 64 字节局部数组 + 50000 次计算（中等函数体）。
 *             - risky_heavy        : 含 64 字节局部数组 + 5000000 次计算（重函数体）。
 *
 *           三个 risky 函数的 canary push/check 开销完全相同，但函数体计算量
 *           依次增大，从而模拟"canary 开销随函数工作量增大而被逐步稀释"的规律，
 *           与真实项目（gzip +0.07%）的测试结果形成理论对应。
 *
 *           通过 BENCHMARK_SCENE 宏选择运行哪组场景：
 *             BENCHMARK_SCENE=1  → risky_light   （理论上界）
 *             BENCHMARK_SCENE=2  → risky_medium  （中间过渡）
 *             BENCHMARK_SCENE=3  → risky_heavy   （接近真实）
 *
 *           编译控制：
 *             所有函数均标记为 __attribute__((noinline)) 以防止编译器内联，
 *             确保 LLVM Pass 能对其独立分析和插桩。返回值用于对抗 DCE（Dead
 *             Code Elimination），防止编译器在高优化级别下将无副作用的计算
 *             完全消除，保证测量结果的有效性。
 *
 * 作者: 陈文嘉
 * 创建日期: 2026-04-15
 */

#include <stdio.h>
#include <stdlib.h>

#ifndef BENCHMARK_SCENE
#define BENCHMARK_SCENE 1
#endif

/* ---------------------------------------------------------------
 * 安全函数：纯算术，无局部数组，不会被插桩
 * --------------------------------------------------------------- */
__attribute__((noinline)) long long safe_math_compute(int n) {
    long long sum = 0;
    for (int i = 0; i < n; i++) {
        sum += (i * i) ^ (i & 0xAA);
    }
    return sum;
}

/* ---------------------------------------------------------------
 * 场景一：轻量函数体
 *   局部数组：64 字节   计算量：64 次循环
 *   canary 开销占函数执行时间比例：最高（理论上界）
 * --------------------------------------------------------------- */
#define OUTER_LOOPS_LIGHT  5000
#define INNER_LOOPS_LIGHT  1000

__attribute__((noinline)) int risky_light(int seed) {
    char buffer[64];
    int checksum = 0;
    for (int i = 0; i < 64; i++) {
        buffer[i] = (char)((i + seed) % 256);
        checksum += buffer[i];
    }
    return checksum;
}

/* ---------------------------------------------------------------
 * 场景二：中等函数体
 *   局部数组：64 字节   计算量：50000 次循环
 *   canary 开销被更多计算稀释，开销百分比明显下降
 * --------------------------------------------------------------- */
#define OUTER_LOOPS_MEDIUM  50
#define INNER_LOOPS_MEDIUM  100

__attribute__((noinline)) int risky_medium(int seed) {
    char buffer[64];
    int checksum = 0;
    /* 先填充缓冲区 */
    for (int i = 0; i < 64; i++) {
        buffer[i] = (char)((i + seed) % 256);
    }
    /* 大量混合计算，模拟真实函数的计算密度 */
    for (int i = 0; i < 50000; i++) {
        checksum += (buffer[i % 64] ^ (i & 0xFF)) * ((i + seed) % 7 + 1);
    }
    return checksum;
}

/* ---------------------------------------------------------------
 * 场景三：重函数体
 *   局部数组：64 字节   计算量：5000000 次循环
 *   canary 开销被大量计算完全稀释，开销接近 0%
 *   对应真实项目（gzip）的测试结果
 * --------------------------------------------------------------- */
#define OUTER_LOOPS_HEAVY  5
#define INNER_LOOPS_HEAVY  10

__attribute__((noinline)) int risky_heavy(int seed) {
    char buffer[64];
    int checksum = 0;
    /* 先填充缓冲区 */
    for (int i = 0; i < 64; i++) {
        buffer[i] = (char)((i + seed) % 256);
    }
    /* 极大量计算，模拟 gzip longest_match 类函数 */
    for (int i = 0; i < 5000000; i++) {
        checksum += (buffer[i % 64] ^ (i & 0xFF)) * ((i + seed) % 7 + 1);
        checksum ^= (checksum >> 3);
    }
    return checksum;
}

/* ---------------------------------------------------------------
 * main：通过 BENCHMARK_SCENE 宏选择运行哪个场景
 * --------------------------------------------------------------- */
int main() {
    long long total_result = 0;

#if BENCHMARK_SCENE == 1
    /* 场景一：轻量函数体，canary 开销最高 */
    for (int i = 0; i < OUTER_LOOPS_LIGHT; i++) {
        for (int j = 0; j < INNER_LOOPS_LIGHT; j++) {
            for (int k = 0; k < 10; k++) {
                total_result += safe_math_compute(100 + k);
            }
            total_result += risky_light(j);
        }
    }
    printf("场景一（轻量）完成。校验和: %lld\n", total_result);

#elif BENCHMARK_SCENE == 2
    /* 场景二：中等函数体，canary 开销中等 */
    for (int i = 0; i < OUTER_LOOPS_MEDIUM; i++) {
        for (int j = 0; j < INNER_LOOPS_MEDIUM; j++) {
            for (int k = 0; k < 10; k++) {
                total_result += safe_math_compute(100 + k);
            }
            total_result += risky_medium(j);
        }
    }
    printf("场景二（中等）完成。校验和: %lld\n", total_result);

#elif BENCHMARK_SCENE == 3
    /* 场景三：重函数体，canary 开销接近 0% */
    for (int i = 0; i < OUTER_LOOPS_HEAVY; i++) {
        for (int j = 0; j < INNER_LOOPS_HEAVY; j++) {
            for (int k = 0; k < 10; k++) {
                total_result += safe_math_compute(100 + k);
            }
            total_result += risky_heavy(j);
        }
    }
    printf("场景三（重量）完成。校验和: %lld\n", total_result);

#else
#error "BENCHMARK_SCENE 必须为 1、2 或 3"
#endif

    return 0;
}
