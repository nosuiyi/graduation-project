/*
 * Copyright 2026 DynamicCanaryProject Authors
 *
 * 文件名: DynamicCanary.cpp
 * 功能描述: 动态金丝雀（Dynamic Canary）LLVM IR 插桩 Pass。
 *
 *           本 Pass 基于 LLVM New Pass Manager（NPM）框架实现，在编译期对
 *           高风险函数自动注入动态 Canary 保护代码，替代传统的静态 Canary。
 *
 *           核心机制：
 *             1. 选择性插桩（shouldInstrument）：
 *                遍历函数的所有 alloca 指令，仅对满足以下任一条件的函数
 *                插桩，避免对安全函数引入不必要的开销：
 *                  a. 含有 ≥ 8 字节的局部数组（缓冲区溢出高风险）。
 *                  b. 含有变长数组（VLA）或动态 alloca（大小运行时确定）。
 *
 *             2. Prologue 插桩（函数入口）：
 *                调用运行时函数 __push_dynamic_canary(FuncID)，生成与当前
 *                函数绑定的随机 Canary 值，存入栈上的 dyn_canary_slot。
 *
 *             3. Epilogue 插桩（每个 return 指令前）：
 *                调用运行时函数 __check_dynamic_canary(FuncID, StoredCanary)，
 *                从影子栈弹出期望值进行比较；若不匹配则触发告警并终止进程。
 *
 *           插件注册：
 *             - 通过 registerPipelineParsingCallback 注册为命名 Pass "dyn-canary"，
 *               可在 opt 中用 -passes="dyn-canary" 单独调用（便于测试）。
 *             - 通过 registerPipelineStartEPCallback 自动挂载到编译器优化流水线
 *               入口（如 clang -O2），实现对真实项目的透明保护。
 *
 * 依赖运行时: runtime/canary_rt.c（__push_dynamic_canary, __check_dynamic_canary）
 * 作者: 陈文嘉
 * 创建日期: 2026-04-15
 */

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/DataLayout.h"

using namespace llvm;

namespace {
    struct DynamicCanaryPass : public PassInfoMixin<DynamicCanaryPass> {
        
        // ==========================================
        // 新增：选择性插桩的判断逻辑
        // ==========================================
        bool shouldInstrument(Function &F) {
            // 获取数据布局对象，用于计算类型在内存中的实际大小
            const DataLayout &DL = F.getParent()->getDataLayout();

            for (BasicBlock &BB : F) {
                for (Instruction &I : BB) {
                    // 检查该指令是否为局部变量分配指令 (alloca)
                    if (AllocaInst *AI = dyn_cast<AllocaInst>(&I)) {
                        Type *AllocType = AI->getAllocatedType();

                        // 风险特征 1：存在数组分配
                        if (AllocType->isArrayTy()) {
                            uint64_t ArrayElements = AllocType->getArrayNumElements();
                            uint64_t ElementSize = DL.getTypeAllocSize(AllocType->getArrayElementType());
                            uint64_t TotalSize = ArrayElements * ElementSize;

                            // 大于等于 8 字节的缓冲区才被认为有溢出风险
                            if (TotalSize >= 8) {
                                errs() << "  -> [触发策略] 在函数 '" << F.getName() << "' 中检测到危险数组 (大小: " << TotalSize << " 字节)\n";
                                return true;
                            }
                        }

                        // 风险特征 2：变长数组 (VLA) 或动态 alloca
                        if (!AI->isStaticAlloca()) {
                            errs() << "  -> [触发策略] 在函数 '" << F.getName() << "' 中检测到动态分配 (VLA/alloca)\n";
                            return true;
                        }
                    }
                }
            }
            // 如果遍历完没有高风险特征，放行（不插桩）
            return false;
        }

        // ==========================================
        // 核心运行函数
        // ==========================================
        PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
            if (F.isDeclaration()) return PreservedAnalyses::all();

            // === 核心拦截点：调用上面的函数进行判断 ===
            
            if (!shouldInstrument(F)) {
                // 如果是安全函数，直接跳过，一行代码都不改！
                return PreservedAnalyses::all();
            }
            
            // 1. 准备工作：获取上下文和类型
            Module *M = F.getParent();
            LLVMContext &Ctx = M->getContext();
            IRBuilder<> Builder(Ctx);

            Type *Int64Ty = Type::getInt64Ty(Ctx);
            Type *VoidTy = Type::getVoidTy(Ctx);

            // 声明外部的运行时函数
            FunctionCallee PushFn = M->getOrInsertFunction("__push_dynamic_canary", Int64Ty, Int64Ty);
            FunctionCallee CheckFn = M->getOrInsertFunction("__check_dynamic_canary", VoidTy, Int64Ty, Int64Ty);

            // 使用 LLVM 内置的字符串哈希函数
            uint64_t FuncID = llvm::hash_value(F.getName());
            Value *FuncIDVal = ConstantInt::get(Int64Ty, FuncID);

            // ==========================================
            // 第一阶段：在函数入口 (Prologue) 插桩
            // ==========================================
            BasicBlock &EntryBB = F.getEntryBlock();
            Instruction *FirstInsertPt = &*EntryBB.getFirstInsertionPt();
            Builder.SetInsertPoint(FirstInsertPt);

            AllocaInst *CanarySlot = Builder.CreateAlloca(Int64Ty, nullptr, "dyn_canary_slot");
            CallInst *GeneratedCanary = Builder.CreateCall(PushFn, {FuncIDVal});
            Builder.CreateStore(GeneratedCanary, CanarySlot);

            // ==========================================
            // 第二阶段：在函数出口 (Epilogue) 插桩
            // ==========================================
            for (BasicBlock &BB : F) {
                Instruction *Term = BB.getTerminator();
                if (isa<ReturnInst>(Term)) {
                    Builder.SetInsertPoint(Term);
                    LoadInst *StoredCanary = Builder.CreateLoad(Int64Ty, CanarySlot);
                    Builder.CreateCall(CheckFn, {FuncIDVal, StoredCanary});
                }
            }

            errs() << "[Dynamic Canary] 成功为危险函数插桩 (New PM): " << F.getName() << "\n";
            
            return PreservedAnalyses::none(); 
        }
    };
}

// ==========================================
// 插件注册机制 (升级为自动注入模式)
// ==========================================
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "DynamicCanaryPass", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            
            // 1. 保留原本的命令行解析能力 (用于 opt 测试)
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "dyn-canary") {
                    FPM.addPass(DynamicCanaryPass());
                    return true;
                  }
                  return false;
                });

            // 2. 新增：自动挂载到编译器的标准优化流水线中 (如 -O2)
            // 这样 clang 在编译真实项目时，会自动调用我们的 Pass
            PB.registerPipelineStartEPCallback(
                [](ModulePassManager &MPM, OptimizationLevel Level) {
                    FunctionPassManager FPM;
                    FPM.addPass(DynamicCanaryPass());
                    MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
                });
          }};
}