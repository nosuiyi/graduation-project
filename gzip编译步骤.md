## 让插件具有“自动注入”的能力
### 将最底部的 llvmGetPassPluginInfo 函数替换为以下代码：
``` C++
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
```

## 编译插件
``` bash
# 1. 重新编译 Pass 插件
cd ~/DynamicCanaryProject/build
make clean && make

# 2. 编译运行时库为目标文件
cd ~/DynamicCanaryProject/runtime
clang -c canary_rt.c -o canary_rt.o
```

## 获取开源项目Gzip
``` bash
cd ~/DynamicCanaryProject
# 下载并解压 gzip 源码
wget https://ftp.gnu.org/gnu/gzip/gzip-1.13.tar.gz
tar -zxvf gzip-1.13.tar.gz
cd gzip-1.13
```

## 环境变量劫持与全自动编译
``` bash
# 1. 获取我们插件和运行时库的绝对路径
export PLUGIN_PATH=$(realpath ../build/libDynamicCanary.so)
export RT_OBJ=$(realpath ../runtime/canary_rt.o)

# 2. 劫持编译器的核心变量！
# 告诉它使用 clang，并自动加载我们的 Pass 插件
export CC="clang -fpass-plugin=$PLUGIN_PATH"

# 使用 -O2 优化，这是真实开源软件的标配
export CFLAGS="-O2"

# 告诉链接器，在最后生成可执行文件时，带上我们的 canary_rt.o
export LDFLAGS="$RT_OBJ"

# 3. 生成 Makefile
./configure

# 4. 开始多线程编译！
make -j4
```

## 测试
### 功能可用性测试
``` bash
# 1. 创建一个测试文本文件
echo "This is a test file for Dynamic Canary protecting the gzip project!" > test_canary.txt

# 2. 检查压缩前的文件大小
ls -l test_canary.txt

# 3. 使用【我们刚刚编译出的受保护版本】进行压缩！
# 注意必须带上 ./ 表示当前目录的 gzip，否则会调用系统的 gzip
./gzip test_canary.txt

# 4. 检查是否成功生成了 test_canary.txt.gz
ls -l test_canary.txt.gz

# 5. 再把它解压回来
./gzip -d test_canary.txt.gz

# 6. 查看文件内容是否完好无损
cat test_canary.txt
```
### 防护真实性验证
``` bash
# 检查最终的 gzip 二进制文件里，是否成功链接了我们的 Canary 运行时函数
nm ./gzip | grep dynamic_canary
```

## 预期结果
``` bash
00000000000xxxxx T __check_dynamic_canary
00000000000xxxxx T __init_dynamic_canary
00000000000xxxxx T __push_dynamic_canary
00000000000xxxxx b thread_local_seed
```