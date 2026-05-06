#!/usr/bin/env bash
# ==============================================================================
# 文件名: build_real_projects.sh
# 功能描述: 为真实开源项目分别编译基线版（无插桩）和保护版（动态 Canary 插桩），
#           并将两个版本的可执行文件保存在项目目录中。
#
#           本脚本只负责编译，不执行性能测试。
#           编译完成后，运行 run_real_perf_test.sh 执行测试。
#
# 支持的项目:
#   - gzip   (gzip-1.13)
#   - bzip2  (bzip2-1.0.8)  [预留，尚未支持]
#
# 用法:
#   bash build_real_projects.sh [项目名]
#
#   不加参数默认编译所有已支持的项目。
#   指定项目名则只编译该项目，例如：bash build_real_projects.sh gzip
#
# 作者: 陈文嘉
# 创建日期: 2026-05-02
# ==============================================================================

set -e  # 任何命令失败时立即退出

# ------------------------------------------------------------------------------
# 路径锚定：无论从哪个目录运行脚本，路径始终正确
# ------------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

PLUGIN_SO="${PROJECT_ROOT}/build/libDynamicCanary.so"
RT_OBJ="${PROJECT_ROOT}/runtime/canary_rt.o"
LOG_OBJ="${PROJECT_ROOT}/runtime/canary_log.o"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

log_info()    { echo -e "${BLUE}[INFO]${NC}  $*"; }
log_ok()      { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn()    { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_error()   { echo -e "${RED}[ERROR]${NC} $*"; }

# ------------------------------------------------------------------------------
# 前置检查：确认插件和运行时库都存在
# ------------------------------------------------------------------------------
check_prerequisites() {
    log_info "检查前置依赖..."

    local MISSING=0

    if [ ! -f "${PLUGIN_SO}" ]; then
        log_error "Pass 插件不存在: ${PLUGIN_SO}"
        log_error "请先在 build/ 目录执行 'make' 重新编译插件。"
        MISSING=1
    fi

    # 如果运行时库 .o 文件不存在，自动编译
    if [ ! -f "${RT_OBJ}" ] || [ ! -f "${LOG_OBJ}" ]; then
        log_warn "运行时库 .o 文件不完整，正在自动编译..."
        (
            cd "${PROJECT_ROOT}/runtime"
            clang -O0 -c canary_rt.c  -o canary_rt.o
            clang -O0 -c canary_log.c -o canary_log.o
        )
        log_ok "运行时库编译完成。"
    fi

    if [ "${MISSING}" -eq 1 ]; then
        exit 1
    fi

    log_ok "前置依赖检查通过。"
    echo ""
}

# ==============================================================================
# gzip 编译函数
# ==============================================================================
build_gzip() {
    local GZIP_DIR="${PROJECT_ROOT}/gzip-1.13"

    echo -e "${BLUE}========================================"
    echo "  编译 gzip-1.13"
    echo -e "========================================${NC}"

    # 检查源码目录
    if [ ! -d "${GZIP_DIR}" ]; then
        log_error "gzip 源码目录不存在: ${GZIP_DIR}"
        log_error "请先解压源码：cd ${PROJECT_ROOT} && tar -zxvf gzip-1.13.tar.gz"
        return 1
    fi

    cd "${GZIP_DIR}"

    # --------------------------------------------------------------------------
    # 第一步：编译基线版（无插桩）
    # --------------------------------------------------------------------------
    if [ -f "${GZIP_DIR}/gzip_baseline" ]; then
        log_warn "基线版本已存在，跳过编译（如需重新编译请先删除 gzip_baseline）。"
    else
        log_info "正在编译基线版 gzip（-O2，无插桩）..."

        make clean 2>/dev/null || true

        export CC="clang"
        export CFLAGS="-O2"
        export LDFLAGS=""

        ./configure --quiet
        make -j4 2>&1 | grep -E "^(Making|clang|error:|warning:)" || true

        cp gzip gzip_baseline
        log_ok "基线版本已保存：$(ls -lh gzip_baseline | awk '{print $5, $9}')"
    fi

    echo ""

    # --------------------------------------------------------------------------
    # 第二步：编译保护版（动态 Canary 插桩）
    # --------------------------------------------------------------------------
    if [ -f "${GZIP_DIR}/gzip_protected" ]; then
        log_warn "保护版本已存在，跳过编译（如需重新编译请先删除 gzip_protected）。"
    else
        log_info "正在编译保护版 gzip（-O2，动态 Canary 插桩）..."
        log_info "（编译过程中会看到插桩日志，属正常现象）"
        echo ""

        make clean 2>/dev/null || true

        export CC="clang -fpass-plugin=${PLUGIN_SO}"
        export CFLAGS="-O2"
        export LDFLAGS="${RT_OBJ} ${LOG_OBJ}"

        ./configure --quiet
        make -j4 2>&1 | tee /tmp/gzip_protected_build.log \
            | grep -E "(\[Dynamic Canary\]|\[触发策略\]|error:)" || true

        cp gzip gzip_protected
        log_ok "保护版本已保存：$(ls -lh gzip_protected | awk '{print $5, $9}')"

        # 统计插桩数量
        local INSTRUMENTED_COUNT
        INSTRUMENTED_COUNT=$(grep -c "成功为危险函数插桩" /tmp/gzip_protected_build.log 2>/dev/null || echo 0)
        log_info "共对 ${INSTRUMENTED_COUNT} 个函数进行了插桩。"

        echo ""
        log_info "被插桩的函数列表："
        grep "成功为危险函数插桩" /tmp/gzip_protected_build.log 2>/dev/null \
            | sed 's/^/    /' || echo "    （未找到插桩日志，请检查 /tmp/gzip_protected_build.log）"
    fi

    echo ""

    # --------------------------------------------------------------------------
    # 第三步：验证插桩有效性
    # --------------------------------------------------------------------------
    log_info "验证插桩有效性..."

    echo "  [保护版] canary 符号："
    nm "${GZIP_DIR}/gzip_protected" 2>/dev/null | grep dynamic_canary \
        | sed 's/^/    /' \
        || log_warn "  保护版本中未找到 canary 符号，请检查编译是否成功。"

    echo "  [基线版] canary 符号（应为空）："
    if nm "${GZIP_DIR}/gzip_baseline" 2>/dev/null | grep -q dynamic_canary; then
        log_warn "  基线版本中意外包含 canary 符号，请检查编译过程。"
    else
        echo "    （正常：基线版本无 canary 符号）"
    fi

    echo ""
    log_ok "gzip 编译完成。"
    echo ""
}

# ==============================================================================
# bzip2 编译函数（预留）
# ==============================================================================
build_bzip2() {
    local BZIP2_DIR="${PROJECT_ROOT}/bzip2-1.0.8"

    echo -e "${BLUE}========================================"
    echo "  编译 bzip2-1.0.8"
    echo -e "========================================${NC}"

    if [ ! -d "${BZIP2_DIR}" ]; then
        log_warn "bzip2 源码目录不存在: ${BZIP2_DIR}"
        log_warn "请先下载并解压源码："
        log_warn "  wget https://sourceware.org/pub/bzip2/bzip2-1.0.8.tar.gz"
        log_warn "  tar -zxvf bzip2-1.0.8.tar.gz -C ${PROJECT_ROOT}"
        return 1
    fi

    cd "${BZIP2_DIR}"

    # 基线版
    if [ -f "${BZIP2_DIR}/bzip2_baseline" ]; then
        log_warn "基线版本已存在，跳过。"
    else
        log_info "正在编译基线版 bzip2..."
        make clean 2>/dev/null || true
        make CC="clang" CFLAGS="-O2" -j4 bzip2 2>&1 | grep -E "^(clang|error:)" || true
        cp bzip2 bzip2_baseline
        log_ok "基线版本已保存：$(ls -lh bzip2_baseline | awk '{print $5, $9}')"
    fi

    echo ""

    # 保护版
    if [ -f "${BZIP2_DIR}/bzip2_protected" ]; then
        log_warn "保护版本已存在，跳过。"
    else
        log_info "正在编译保护版 bzip2..."
        make clean 2>/dev/null || true
        make CC="clang -fpass-plugin=${PLUGIN_SO}" \
             CFLAGS="-O2" \
             LDFLAGS="${RT_OBJ} ${LOG_OBJ}" \
             -j4 bzip2 2>&1 | tee /tmp/bzip2_protected_build.log \
             | grep -E "(\[Dynamic Canary\]|\[触发策略\]|error:)" || true
        cp bzip2 bzip2_protected
        log_ok "保护版本已保存：$(ls -lh bzip2_protected | awk '{print $5, $9}')"

        local INSTRUMENTED_COUNT
        INSTRUMENTED_COUNT=$(grep -c "成功为危险函数插桩" /tmp/bzip2_protected_build.log 2>/dev/null || echo 0)
        log_info "共对 ${INSTRUMENTED_COUNT} 个函数进行了插桩。"
    fi

    echo ""
    log_ok "bzip2 编译完成。"
    echo ""
}

# ==============================================================================
# 入口：根据参数决定编译哪些项目
# ==============================================================================
main() {
    local TARGET="${1:-all}"

    echo ""
    echo -e "${BLUE}=================================================="
    echo "  动态 Canary 真实项目编译脚本"
    echo -e "==================================================${NC}"
    echo "  项目根目录: ${PROJECT_ROOT}"
    echo "  Pass 插件:  ${PLUGIN_SO}"
    echo "  运行时库:   ${RT_OBJ}"
    echo "              ${LOG_OBJ}"
    echo ""

    check_prerequisites

    case "${TARGET}" in
        gzip)
            build_gzip
            ;;
        bzip2)
            build_bzip2
            ;;
        all)
            build_gzip
            build_bzip2
            ;;
        *)
            log_error "未知项目名: ${TARGET}"
            echo "  支持的项目名: gzip | bzip2 | all"
            exit 1
            ;;
    esac

    echo -e "${GREEN}=================================================="
    echo "  所有项目编译完成"
    echo -e "==================================================${NC}"
    echo "  接下来运行性能测试："
    echo "    bash ${SCRIPT_DIR}/run_real_perf_test.sh [gzip|bzip2|all]"
    echo ""
}

main "$@"
