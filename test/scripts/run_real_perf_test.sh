#!/usr/bin/env bash
# ==============================================================================
# 文件名: run_real_perf_test.sh
# 功能描述: 对已编译好的真实开源项目（基线版 vs 保护版）进行性能对比测试，
#           量化动态 Canary 插桩带来的执行时间和峰值内存开销。
#
#           本脚本只负责测试，不执行编译。
#           请先运行 build_real_projects.sh 完成编译，再执行本脚本。
#
# 测试维度:
#   1. 执行时间开销：30次取均值（去掉前后各10%极值），单位：秒
#   2. 峰值内存开销：5次取均值（RSS），单位：KB
#
# 支持的项目:
#   - gzip   (gzip-1.13)
#   - bzip2  (bzip2-1.0.8)
#
# 用法:
#   bash run_real_perf_test.sh [项目名]
#
#   不加参数默认测试所有已支持的项目。
#   指定项目名则只测试该项目，例如：bash run_real_perf_test.sh gzip
#
# 作者: 陈文嘉
# 创建日期: 2026-05-02
# ==============================================================================

# ------------------------------------------------------------------------------
# 路径锚定
# ------------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# ------------------------------------------------------------------------------
# 测试配置（可按需修改）
# ------------------------------------------------------------------------------
TIME_RUNS=30      # 执行时间测试重复次数
MEMORY_RUNS=5     # 内存峰值测试重复次数

# 测试文件大小（MB）
TEST_FILE_SIZE_MB=200

# 临时文件路径
TIME_STAT_FILE="/tmp/canary_time_stat.txt"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

log_info()  { echo -e "${BLUE}[INFO]${NC}  $*"; }
log_ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }

# ==============================================================================
# 工具函数
# ==============================================================================

# 检查 /usr/bin/time -v 是否可用
check_time_tool() {
    if ! /usr/bin/time -v true 2>/dev/null 1>/dev/null; then
        log_error "/usr/bin/time -v 不可用，无法测量内存。"
        log_error "请执行: sudo apt install time"
        return 1
    fi
    return 0
}

# 准备测试数据文件（若已存在且大小正确则跳过）
prepare_test_file() {
    local FILE_PATH="$1"
    local SIZE_MB="$2"
    local EXPECTED_SIZE=$(( SIZE_MB * 1024 * 1024 ))

    if [ -f "${FILE_PATH}" ]; then
        local ACTUAL_SIZE
        ACTUAL_SIZE=$(stat -c%s "${FILE_PATH}" 2>/dev/null || echo 0)
        if [ "${ACTUAL_SIZE}" -ge "${EXPECTED_SIZE}" ]; then
            log_info "测试文件已存在，跳过生成：${FILE_PATH} ($(du -sh "${FILE_PATH}" | cut -f1))"
            return 0
        fi
    fi

    log_info "正在生成 ${SIZE_MB}MB 随机测试文件：${FILE_PATH} ..."
    dd if=/dev/urandom of="${FILE_PATH}" bs=1M count="${SIZE_MB}" 2>/dev/null
    log_ok "测试文件已生成：$(ls -lh "${FILE_PATH}" | awk '{print $5, $9}')"
}

# 测量执行时间（多次取均值，去掉前后各 10% 极值）
# 参数: $1=二进制路径  $2=参数字符串  $3=重复次数
# 返回: 平均时间（秒，4位小数）写入 stdout
measure_time() {
    local BIN="$1"
    local ARGS="$2"
    local RUNS="$3"

    local TIMES=()
    for (( i=1; i<=RUNS; i++ )); do
        local T
        T=$( { /usr/bin/time -f "%e" ${BIN} ${ARGS} > /dev/null; } 2>&1 )
        TIMES+=("${T}")
        printf "    第 %2d/%d 次: %s 秒\n" "${i}" "${RUNS}" "${T}" >&2
    done

    # 排序后去掉前后各 10% 极值，对剩余样本取均值
    printf '%s\n' "${TIMES[@]}" | sort -n | awk -v runs="${RUNS}" '
    {
        vals[NR] = $1
        total = NR
    }
    END {
        trim = int(runs * 0.1)
        if (trim < 1) trim = 1
        sum = 0; count = 0
        for (i = trim+1; i <= total-trim; i++) {
            sum += vals[i]
            count++
        }
        printf "%.4f", sum/count
    }'
}

# 测量峰值内存（RSS，多次取均值）
# 参数: $1=二进制路径  $2=参数字符串  $3=重复次数
# 返回: 平均 RSS (KB) 写入 stdout
measure_memory() {
    local BIN="$1"
    local ARGS="$2"
    local RUNS="$3"

    local RSS_VALUES=()
    for (( i=1; i<=RUNS; i++ )); do
        /usr/bin/time -v ${BIN} ${ARGS} > /dev/null 2>"${TIME_STAT_FILE}"
        local RSS
        RSS=$(grep "Maximum resident" "${TIME_STAT_FILE}" | awk '{print $NF}')
        if [ -n "${RSS}" ]; then
            RSS_VALUES+=("${RSS}")
            printf "    第 %2d/%d 次: %s KB\n" "${i}" "${RUNS}" "${RSS}" >&2
        fi
    done

    if [ ${#RSS_VALUES[@]} -eq 0 ]; then
        echo "-1"
        return
    fi

    printf '%s\n' "${RSS_VALUES[@]}" | awk '
    { sum += $1; count++ }
    END { printf "%d", sum/count }'
}

# 计算开销百分比
calc_overhead() {
    local BASE="$1"
    local PROT="$2"
    python3 -c "
base = float('${BASE}')
prot = float('${PROT}')
if base == 0:
    print('N/A')
else:
    overhead = (prot - base) / base * 100
    sign = '+' if overhead >= 0 else ''
    print(f'{sign}{overhead:.2f}%')
"
}

# ==============================================================================
# gzip 性能测试
# ==============================================================================
test_gzip() {
    local GZIP_DIR="${PROJECT_ROOT}/gzip-1.13"
    local BASELINE="${GZIP_DIR}/gzip_baseline"
    local PROTECTED="${GZIP_DIR}/gzip_protected"
    local TEST_FILE="/tmp/test_gzip_${TEST_FILE_SIZE_MB}mb.bin"

    echo ""
    echo -e "${CYAN}=================================================="
    echo "  gzip-1.13 性能测试"
    echo -e "==================================================${NC}"

    # 检查二进制是否存在
    if [ ! -f "${BASELINE}" ] || [ ! -f "${PROTECTED}" ]; then
        log_error "编译产物不完整，请先运行："
        log_error "  bash ${SCRIPT_DIR}/build_real_projects.sh gzip"
        return 1
    fi

    # 准备测试文件
    prepare_test_file "${TEST_FILE}" "${TEST_FILE_SIZE_MB}"
    echo ""

    # 功能验证
    log_info "功能验证..."
    if ${BASELINE}  -k "${TEST_FILE}" -c > /dev/null 2>&1; then
        log_ok "基线版本：压缩正常"
    else
        log_error "基线版本功能异常，请检查二进制。"
        return 1
    fi
    if ${PROTECTED} -k "${TEST_FILE}" -c > /dev/null 2>&1; then
        log_ok "保护版本：压缩正常"
    else
        log_error "保护版本功能异常，请检查二进制。"
        return 1
    fi
    echo ""

    # 压缩命令参数（-k 保留原文件，-c 输出到 stdout）
    local GZIP_ARGS="-k ${TEST_FILE} -c"

    # ------------------------------------------------------------------
    # 执行时间测试
    # ------------------------------------------------------------------
    echo -e "${BLUE}--- 执行时间测试（各 ${TIME_RUNS} 次）---${NC}"
    echo ""

    echo "  [基线版本]"
    BASE_TIME=$(measure_time "${BASELINE}" "${GZIP_ARGS}" "${TIME_RUNS}")

    echo ""
    echo "  [保护版本]"
    PROT_TIME=$(measure_time "${PROTECTED}" "${GZIP_ARGS}" "${TIME_RUNS}")

    echo ""
    local TIME_OVERHEAD
    TIME_OVERHEAD=$(calc_overhead "${BASE_TIME}" "${PROT_TIME}")

    echo -e "  ${GREEN}基线版本平均耗时: ${BASE_TIME} 秒${NC}"
    echo -e "  ${GREEN}保护版本平均耗时: ${PROT_TIME} 秒${NC}"
    echo -e "  ${GREEN}执行时间开销:     ${TIME_OVERHEAD}${NC}"

    # ------------------------------------------------------------------
    # 内存峰值测试
    # ------------------------------------------------------------------
    echo ""
    echo -e "${BLUE}--- 内存峰值测试（各 ${MEMORY_RUNS} 次）---${NC}"
    echo ""

    if ! check_time_tool; then
        log_warn "跳过内存测试。"
    else
        echo "  [基线版本]"
        BASE_MEM=$(measure_memory "${BASELINE}" "${GZIP_ARGS}" "${MEMORY_RUNS}")

        echo ""
        echo "  [保护版本]"
        PROT_MEM=$(measure_memory "${PROTECTED}" "${GZIP_ARGS}" "${MEMORY_RUNS}")

        echo ""
        if [ "${BASE_MEM}" -gt 0 ] && [ "${PROT_MEM}" -gt 0 ]; then
            local MEM_OVERHEAD
            MEM_OVERHEAD=$(calc_overhead "${BASE_MEM}" "${PROT_MEM}")
            local BASE_MB PROT_MB
            BASE_MB=$(python3 -c "print(f'{${BASE_MEM}/1024:.2f}')")
            PROT_MB=$(python3 -c "print(f'{${PROT_MEM}/1024:.2f}')")

            echo -e "  ${GREEN}基线版本峰值内存: ${BASE_MEM} KB (${BASE_MB} MB)${NC}"
            echo -e "  ${GREEN}保护版本峰值内存: ${PROT_MEM} KB (${PROT_MB} MB)${NC}"
            echo -e "  ${GREEN}内存开销:         ${MEM_OVERHEAD}${NC}"
        else
            log_warn "内存数据获取失败。"
        fi
    fi

    # ------------------------------------------------------------------
    # gzip 小结
    # ------------------------------------------------------------------
    echo ""
    echo -e "${CYAN}--- gzip-1.13 测试结果汇总 ---${NC}"
    printf "  %-20s %s 秒\n" "基线执行时间:" "${BASE_TIME}"
    printf "  %-20s %s 秒\n" "保护执行时间:" "${PROT_TIME}"
    printf "  %-20s %s\n"    "执行时间开销:" "${TIME_OVERHEAD}"
    if [ "${BASE_MEM:-0}" -gt 0 ]; then
        printf "  %-20s %s KB\n" "基线峰值内存:" "${BASE_MEM}"
        printf "  %-20s %s KB\n" "保护峰值内存:" "${PROT_MEM}"
        printf "  %-20s %s\n"    "内存开销:"     "${MEM_OVERHEAD}"
    fi
    echo ""
}

# ==============================================================================
# bzip2 性能测试（预留，结构与 gzip 一致）
# ==============================================================================
test_bzip2() {
    local BZIP2_DIR="${PROJECT_ROOT}/bzip2-1.0.8"
    local BASELINE="${BZIP2_DIR}/bzip2_baseline"
    local PROTECTED="${BZIP2_DIR}/bzip2_protected"
    local TEST_FILE="/tmp/test_bzip2_${TEST_FILE_SIZE_MB}mb.bin"

    echo ""
    echo -e "${CYAN}=================================================="
    echo "  bzip2-1.0.8 性能测试"
    echo -e "==================================================${NC}"

    if [ ! -f "${BASELINE}" ] || [ ! -f "${PROTECTED}" ]; then
        log_error "编译产物不完整，请先运行："
        log_error "  bash ${SCRIPT_DIR}/build_real_projects.sh bzip2"
        return 1
    fi

    prepare_test_file "${TEST_FILE}" "${TEST_FILE_SIZE_MB}"
    echo ""

    # bzip2 压缩参数（-k 保留原文件，-c 输出到 stdout）
    local BZIP2_ARGS="-k ${TEST_FILE} -c"

    echo -e "${BLUE}--- 执行时间测试（各 ${TIME_RUNS} 次）---${NC}"
    echo ""

    echo "  [基线版本]"
    BASE_TIME=$(measure_time "${BASELINE}" "${BZIP2_ARGS}" "${TIME_RUNS}")

    echo ""
    echo "  [保护版本]"
    PROT_TIME=$(measure_time "${PROTECTED}" "${BZIP2_ARGS}" "${TIME_RUNS}")

    echo ""
    local TIME_OVERHEAD
    TIME_OVERHEAD=$(calc_overhead "${BASE_TIME}" "${PROT_TIME}")
    echo -e "  ${GREEN}基线版本平均耗时: ${BASE_TIME} 秒${NC}"
    echo -e "  ${GREEN}保护版本平均耗时: ${PROT_TIME} 秒${NC}"
    echo -e "  ${GREEN}执行时间开销:     ${TIME_OVERHEAD}${NC}"

    echo ""
    echo -e "${BLUE}--- 内存峰值测试（各 ${MEMORY_RUNS} 次）---${NC}"
    echo ""

    if check_time_tool; then
        echo "  [基线版本]"
        BASE_MEM=$(measure_memory "${BASELINE}" "${BZIP2_ARGS}" "${MEMORY_RUNS}")

        echo ""
        echo "  [保护版本]"
        PROT_MEM=$(measure_memory "${PROTECTED}" "${BZIP2_ARGS}" "${MEMORY_RUNS}")

        echo ""
        if [ "${BASE_MEM}" -gt 0 ] && [ "${PROT_MEM}" -gt 0 ]; then
            local MEM_OVERHEAD
            MEM_OVERHEAD=$(calc_overhead "${BASE_MEM}" "${PROT_MEM}")
            local BASE_MB PROT_MB
            BASE_MB=$(python3 -c "print(f'{${BASE_MEM}/1024:.2f}')")
            PROT_MB=$(python3 -c "print(f'{${PROT_MEM}/1024:.2f}')")
            echo -e "  ${GREEN}基线版本峰值内存: ${BASE_MEM} KB (${BASE_MB} MB)${NC}"
            echo -e "  ${GREEN}保护版本峰值内存: ${PROT_MEM} KB (${PROT_MB} MB)${NC}"
            echo -e "  ${GREEN}内存开销:         ${MEM_OVERHEAD}${NC}"
        fi
    fi

    echo ""
    echo -e "${CYAN}--- bzip2-1.0.8 测试结果汇总 ---${NC}"
    printf "  %-20s %s 秒\n" "基线执行时间:" "${BASE_TIME}"
    printf "  %-20s %s 秒\n" "保护执行时间:" "${PROT_TIME}"
    printf "  %-20s %s\n"    "执行时间开销:" "${TIME_OVERHEAD}"
    if [ "${BASE_MEM:-0}" -gt 0 ]; then
        printf "  %-20s %s KB\n" "基线峰值内存:" "${BASE_MEM}"
        printf "  %-20s %s KB\n" "保护峰值内存:" "${PROT_MEM}"
        printf "  %-20s %s\n"    "内存开销:"     "${MEM_OVERHEAD}"
    fi
    echo ""
}

# ==============================================================================
# 入口
# ==============================================================================
main() {
    local TARGET="${1:-all}"

    echo ""
    echo -e "${BLUE}=================================================="
    echo "  动态 Canary 真实项目性能测试脚本"
    echo -e "==================================================${NC}"
    echo "  项目根目录: ${PROJECT_ROOT}"
    echo "  执行时间测试次数: ${TIME_RUNS}（去掉前后各 10% 极值后取均值）"
    echo "  内存测试次数:     ${MEMORY_RUNS}"
    echo "  测试文件大小:     ${TEST_FILE_SIZE_MB} MB 随机数据"
    echo ""

    case "${TARGET}" in
        gzip)
            test_gzip
            ;;
        bzip2)
            test_bzip2
            ;;
        all)
            test_gzip
            test_bzip2
            ;;
        *)
            log_error "未知项目名: ${TARGET}"
            echo "  支持的项目名: gzip | bzip2 | all"
            exit 1
            ;;
    esac

    echo -e "${GREEN}=================================================="
    echo "  测试完成"
    echo -e "==================================================${NC}"
    echo ""
}

main "$@"
