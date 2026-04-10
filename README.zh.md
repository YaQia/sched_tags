# 标签化调度优化器

## 编译

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## 使用方法

使用 `clang` 编译时，通过 `-fpass-plugin` 加载编译出的 Pass 插件即可对 C/C++ 项目进行标签插桩。默认情况下，Pass 会读取当前目录下的 `sched_tags.json` 文件。

```bash
# 基本使用（以 C 语言为例）
clang -O3 -fpass-plugin=build/pass/SchedTagPass.so -c input.c -o output.o

# 禁用 Pass 自动分析插桩
# 如果只需要通过 sched_tags.json 显式配置标签，而不需要 Pass 自动去分析代码特征（如自动识别计算密集或原子操作等），
# 可以传入 -mllvm -sched-auto-analysis=false 参数来禁用自动分析。
clang -O3 -fpass-plugin=build/pass/SchedTagPass.so -mllvm -sched-auto-analysis=false -c input.c -o output.o
```

> **注意**：不同操作系统下插件扩展名不同，Linux 下为 `.so`，macOS 下通常为 `.dylib`。

## 包含标签

### 1. 指令特性标签

| 标签类型        | 使用场景                                             | 值                                       | 调度目标                                                                                                            |
| --------------- | ---------------------------------------------------- | ---------------------------------------- | ------------------------------------------------------------------------------------------------------------------- |
| `compute-dense` | 计算密集代码片段（大量整数/浮点/SIMD运算）           | INT/FLOAT/SIMD（或者按位或将这些值合并） | 选择计算资源空闲的核心，优化计算吞吐                                                                                |
| `branch-dense`  | 分支密集代码（大量条件跳转）                         | 1                                        | 防止多个分支密集任务在同核心的 SMT 上运行                                                                           |
| `memory-dense`  | 内存访问密集代码（大量 load/store）                  | STREAM/RANDOM                            | 优化 CPU 亲和性，保证在缓存热的核心上运行                                                                           |
| `atomic-dense`  | 原子操作密集且计算逻辑极简的代码（CAS 循环、自旋锁） | 1                                        | 将访问相同原子变量的线程调度到相同核心或SMT核，避免MESI协议在原子指令中冲刷缓存带来的开销（用并发度换取缓存命中率） |

### 2. 资源需求标签

| 标签类型   | 使用场景         | 值  | 调度目标                                      |
| ---------- | ---------------- | --- | --------------------------------------------- |
| `io-dense` | I/O 密集代码片段 | 1   | 保证任务以较高优先级被调度，但忽略 CPU 亲和性 |
| `unshared` | 独占资源被占用时 | 1   | 在拥有该标记时禁止抢占，防止优先级反转        |

### 3. 行为预测标签

| 标签类型       | 使用场景           | 值  | 调度目标                                       |
| -------------- | ------------------ | --- | ---------------------------------------------- |
| `compute-prep` | 负载即将快速上升   | 1   | 主动抬升 CPU 频率，不依赖负载检测升频          |
| `dependency`   | IPC 通信中保存魔数 | 1   | 将魔数赋值给标签，相同依赖的代码块会有相同魔数 |

## SchedQL 查询语言

定义见[schedql.ebnf](./schedql.ebnf)

### TODO List

- [ ] `loop[in=Type]` 模式
- [ ] `loop[not_in=Type]` 模式
- [x] `func=name` 谓词（调用特定函数）
- [x] `var=name` 谓词（使用特定变量）
- [ ] 函数签名精确匹配
- [ ] 实现基于后向支配树（Post-Dominator Tree）的模块级控制流分析，确保 `unshared` 等标签的 `start` 和 `end` 范围完全封闭，避免提前 `return` 或异常导致标签逃逸。
- [ ] 处理异常展开（Exception Unwinding）时的标签清理（TLS 清空）。
