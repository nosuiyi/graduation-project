# Copyright 2026 DynamicCanaryProject Authors
#
# 文件名: perf_test.py
# 功能描述: 动态金丝雀防御方案性能消耗基准测试脚本。
#
#           本脚本对基线版本（无保护）与保护版本（动态 Canary 插桩）进行
#           全方位性能对比，量化防御机制引入的额外开销，涵盖四个维度：
#             1. 编译时间开销：插桩 Pass 对编译流程的耗时影响
#             2. 二进制体积开销：插桩后可执行文件的体积增长
#             3. 运行时执行时间开销：程序运行速度的影响
#             4. 运行时内存开销：峰值物理内存（RSS）的增长
#
#           测试场景（通过 BENCHMARK_SCENE 宏区分）：
#             场景一（SCENE=1）：轻量函数体，canary 开销占比最高（理论上界）
#             场景二（SCENE=2）：中等函数体，canary 开销中间过渡值
#             场景三（SCENE=3）：重量函数体，canary 开销接近 0%（对应真实项目）
#
#           通过梯度对比，说明"被插桩函数工作量越大，相对开销越低"的理论规律，
#           与真实项目（gzip +0.07%）的测试结果形成呼应。
#
#           测试方法：
#             - 使用同一份 LLVM IR 源文件（消除编译差异干扰）
#             - 每项测试重复多次取平均值（减少系统抖动误差）
#             - 使用 /usr/bin/time -v 读取 RSS 作为内存峰值依据
#
# 依赖工具: clang, opt, /proc 文件系统（Linux 专有）
# 作者: 陈文嘉
# 创建日期: 2026-04-15

import os
import subprocess
import time

# ============================================================
# 测试配置常量
# ============================================================

# 时间测试的重复运行次数，次数越多结果越稳定，但总耗时也越长
TIME_TEST_RUNS = 200

# 内存测试的重复运行次数
MEMORY_TEST_RUNS = 5

# 编译时间测试的重复次数
COMPILE_TEST_RUNS = 3

# 各文件路径常量，集中定义便于维护
PATH_SRC_BENCHMARK  = "../src/benchmark.c"
PATH_PASS_PLUGIN    = "../../build/libDynamicCanary.so"
PATH_CANARY_RT      = "../../runtime/canary_rt.o"
PATH_CANARY_LOG     = "../../runtime/canary_log.o"

# 三个测试场景的描述信息
SCENES = [
    {
        "id":    1,
        "label": "场景一：轻量函数体（64 次循环）",
        "desc":  "canary 开销占比最高，体现理论上界",
    },
    {
        "id":    2,
        "label": "场景二：中等函数体（50000 次循环）",
        "desc":  "canary 开销被更多计算稀释，开销明显下降",
    },
    {
        "id":    3,
        "label": "场景三：重量函数体（5000000 次循环）",
        "desc":  "canary 开销趋近于 0%，对应真实项目（gzip）行为",
    },
]

# ============================================================
# 工具函数
# ============================================================

def measure_execution_time(binary_path, runs=TIME_TEST_RUNS):
    """多次运行目标程序，返回平均执行时间（秒）。

    为消除系统调度抖动的影响，丢弃最高和最低值后取平均（当 runs >= 5 时）。

    Args:
        binary_path: 目标可执行文件的路径字符串。
        runs: 重复运行次数，默认为 TIME_TEST_RUNS。

    Returns:
        平均执行时间，单位为秒（float）。
    """
    times = []
    for _ in range(runs):
        start = time.time()
        subprocess.run(
            [binary_path],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL
        )
        end = time.time()
        times.append(end - start)

    if runs >= 5:
        times.remove(max(times))
        times.remove(min(times))

    # times.sort()
    # trim = max(1,int(len(times)*0.1))
    # trimmed = times[trim:-trim]
    return sum(times) / len(times)
    # return sum(trimmed) / len(trimmed)


def measure_peak_memory_kb(binary_path, runs=MEMORY_TEST_RUNS):
    """多次运行目标程序，返回峰值物理内存使用量的平均值（KB）。

    实现原理：
      通过 /usr/bin/time -v 工具运行目标程序，解析其输出中的
      "Maximum resident set size" 字段，即操作系统记录的进程
      峰值物理内存（RSS）占用。

    Args:
        binary_path: 目标可执行文件的路径字符串。
        runs: 重复运行次数，默认为 MEMORY_TEST_RUNS。

    Returns:
        峰值 RSS 的平均值，单位为 KB（float）。
        若测量失败（如 /usr/bin/time 不可用），返回 -1.0。
    """
    rss_values = []
    for _ in range(runs):
        try:
            result = subprocess.run(
                ["/usr/bin/time", "-v", binary_path],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
                text=True
            )
            for line in result.stderr.splitlines():
                if "Maximum resident set size" in line:
                    rss_kb = int(line.strip().split(":")[-1].strip())
                    rss_values.append(rss_kb)
                    break
        except (FileNotFoundError, ValueError):
            return -1.0

    if not rss_values:
        return -1.0

    return sum(rss_values) / len(rss_values)


def measure_compile_time_seconds(compile_cmd, runs=COMPILE_TEST_RUNS):
    """多次执行编译命令，返回平均编译耗时（秒）。

    Args:
        compile_cmd: 编译命令字符串，将通过 shell 执行。
        runs: 重复编译次数，默认为 COMPILE_TEST_RUNS。

    Returns:
        平均编译时间，单位为秒（float）。
    """
    times = []
    for _ in range(runs):
        start = time.time()
        os.system(compile_cmd + " 2>/dev/null")
        end = time.time()
        times.append(end - start)
    return sum(times) / len(times)


def format_overhead(base, prot):
    """计算并格式化开销百分比字符串。

    Args:
        base: 基线版本的测量值（数值类型）。
        prot: 保护版本的测量值（数值类型）。

    Returns:
        格式化的开销字符串，例如 "+12.34%" 或 "-0.56%"。
    """
    if base == 0:
        return "N/A（基线值为零）"
    overhead = ((prot - base) / base) * 100
    sign = "+" if overhead >= 0 else ""
    return f"{sign}{overhead:.2f}%"


def run_scene(scene):
    """对单个场景执行完整的四维性能测试。

    Args:
        scene: SCENES 列表中的一项字典，包含 id / label / desc。

    Returns:
        包含本场景所有测量结果的字典。
    """
    sid = scene["id"]
    define_flag = f"-DBENCHMARK_SCENE={sid}"

    path_raw_ir    = f"../output/benchmark_scene{sid}_raw.ll"
    path_prot_ir   = f"../output/benchmark_scene{sid}_prot.ll"
    path_baseline  = f"../output/baseline_scene{sid}"
    path_protected = f"../output/protected_scene{sid}"

    print(f"\n{'=' * 52}")
    print(f"  {scene['label']}")
    print(f"  {scene['desc']}")
    print(f"{'=' * 52}")

    # --------------------------------------------------
    # 准备：生成统一的 LLVM IR
    # --------------------------------------------------
    print(f"\n[准备] 生成 LLVM IR（BENCHMARK_SCENE={sid}）...")
    os.system(
        f"clang -O1 -Xclang -disable-O0-optnone {define_flag} "
        f"-S -emit-llvm {PATH_SRC_BENCHMARK} -o {path_raw_ir} 2>/dev/null"
    )

    # --------------------------------------------------
    # 阶段一：编译时间
    # --------------------------------------------------
    print(f"[阶段一] 编译时间测试（各 {COMPILE_TEST_RUNS} 次）...")

    cmd_base = f"clang -O1 {define_flag} {path_raw_ir} -o {path_baseline}"
    compile_time_base = measure_compile_time_seconds(cmd_base)

    cmd_opt   = (
        f"opt -S -load-pass-plugin={PATH_PASS_PLUGIN} "
        f"-passes=\"dyn-canary\" < {path_raw_ir} > {path_prot_ir}"
    )
    cmd_clang = (
        f"clang {path_prot_ir} {PATH_CANARY_RT} {PATH_CANARY_LOG} "
        f"-o {path_protected}"
    )
    compile_time_prot = measure_compile_time_seconds(cmd_opt + " && " + cmd_clang)

    print(f"  基线编译耗时: {compile_time_base:.3f} 秒")
    print(f"  保护编译耗时: {compile_time_prot:.3f} 秒")
    print(f"  编译时间开销: {format_overhead(compile_time_base, compile_time_prot)}")

    # --------------------------------------------------
    # 阶段二：二进制体积
    # --------------------------------------------------
    size_base = os.path.getsize(path_baseline)
    size_prot = os.path.getsize(path_protected)
    print(f"\n[阶段二] 二进制体积")
    print(f"  基线体积: {size_base:,} 字节  ({size_base / 1024:.1f} KB)")
    print(f"  保护体积: {size_prot:,} 字节  ({size_prot / 1024:.1f} KB)")
    print(f"  体积开销: {format_overhead(size_base, size_prot)}")

    # --------------------------------------------------
    # 阶段三：执行时间
    # --------------------------------------------------
    print(f"\n[阶段三] 执行时间测试（各 {TIME_TEST_RUNS} 次，去最大最小后取均值）...")
    print("  [*] 基线版本...")
    time_base = measure_execution_time(path_baseline)
    print("  [*] 保护版本...")
    time_prot = measure_execution_time(path_protected)
    print(f"  基线平均耗时: {time_base:.4f} 秒")
    print(f"  保护平均耗时: {time_prot:.4f} 秒")
    print(f"  执行时间开销: {format_overhead(time_base, time_prot)}")

    # --------------------------------------------------
    # 阶段四：内存峰值
    # --------------------------------------------------
    print(f"\n[阶段四] 内存峰值测试（各 {MEMORY_TEST_RUNS} 次）...")
    print("  [*] 基线版本...")
    mem_base = measure_peak_memory_kb(path_baseline)
    print("  [*] 保护版本...")
    mem_prot = measure_peak_memory_kb(path_protected)

    if mem_base < 0 or mem_prot < 0:
        print("  [!] 内存测量失败：未找到 /usr/bin/time 工具")
        mem_overhead_str = "N/A"
    else:
        print(f"  基线峰值内存: {mem_base:.0f} KB  ({mem_base / 1024:.2f} MB)")
        print(f"  保护峰值内存: {mem_prot:.0f} KB  ({mem_prot / 1024:.2f} MB)")
        print(f"  内存开销:     {format_overhead(mem_base, mem_prot)}")
        mem_overhead_str = format_overhead(mem_base, mem_prot)

    return {
        "scene":              scene["label"],
        "desc":               scene["desc"],
        "compile_base":       compile_time_base,
        "compile_prot":       compile_time_prot,
        "compile_overhead":   format_overhead(compile_time_base, compile_time_prot),
        "size_base":          size_base,
        "size_prot":          size_prot,
        "size_overhead":      format_overhead(size_base, size_prot),
        "time_base":          time_base,
        "time_prot":          time_prot,
        "time_overhead":      format_overhead(time_base, time_prot),
        "mem_base":           mem_base,
        "mem_prot":           mem_prot,
        "mem_overhead":       mem_overhead_str,
    }


# ============================================================
# 主测试流程
# ============================================================

print("\n" + "=" * 52)
print("  动态 Canary 性能消耗基准测试（梯度场景）")
print("=" * 52)
print(f"  时间测试轮数: {TIME_TEST_RUNS}（去最大最小后取均值）")
print(f"  内存测试轮数: {MEMORY_TEST_RUNS}")
print(f"  编译测试轮数: {COMPILE_TEST_RUNS}")

results = []
for scene in SCENES:
    result = run_scene(scene)
    results.append(result)

# ============================================================
# 汇总对比报告
# ============================================================

print("\n\n" + "=" * 76)
print("  梯度测试汇总对比")
print("=" * 76)

header = (
    f"{'场景':<6}   "
    f"{'编译时间开销':>12}  "
    f"{'体积开销':>8}   "  
    f"{'执行时间开销':>12}  "  
    f"{'内存开销':>8}"
)
sep    = "-" * len(header)
print(header)
print(sep)

for r in results:
    # 场景标签截短以对齐
    label = r["scene"].split("：")[0]   # "场景一"
    print(
        f"{label:<6}  "
        f"{r['desc']:<26}   "
        f"{r['compile_overhead']:>12}  "
        f"{r['size_overhead']:>8}  "
        f"{r['time_overhead']:>12}  "
        f"{r['mem_overhead']:>8}"
    )

print(sep)
print("\n说明：从场景一到场景三，被插桩函数体计算量依次增大，")
print("      canary push/check 绝对开销不变，相对开销随之下降，")
# print("      场景三结果与真实项目（gzip +0.07%）的规律一致。")
print("=" * 76 + "\n")
