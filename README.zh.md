# 标签化调度优化器

## 编译

```bash
cmake -B build
cmake --build build -j$(nproc)
```

本项目依赖内核头文件 `linux/sched_hint.h`。如果本机已安装 `linux-headers` 且其内核版本已支持 sched_hint，则头文件已在默认路径中，无需任何额外配置。否则需通过 `KERNEL_HEADERS` 指定该头文件所在的 include 目录：

```bash
cmake -B build -DKERNEL_HEADERS=/path/to/kernel/usr/include
cmake --build build -j$(nproc)
```

> 手动用 `clang` 编译被插桩代码或 header-only 使用时，对应地加 `-I /path/to/kernel/usr/include`。

## 使用方法

你可以通过 LLVM Pass 在编译期自动插桩，也可以直接在源码中引入头文件进行手动插桩。这两种方式可以混合使用，也可单独使用。

### 方式一：源码级手动插桩 (Header-only)

我们提供了一个轻量级的 C/C++ 头文件 `include/sched_tag.h`，它完全独立于 LLVM Pass。你只需要在代码中包含它，即可直接对代码块打上调度标签。它兼容原生的 GCC 和 Clang 编译器。

```c
#include "sched_tag.h"

void process_data() {
    // 1. 基础标签分配：进入区域前置为对应枚举值，离开置 NONE
    sched_tag(UNSHARED, SCHED_UNSHARED_HELD);
    critical_logic();
    sched_tag(UNSHARED, SCHED_UNSHARED_NONE);

    // 2. 带有变量依赖的共调度（计算指针地址的 Bloom Filter 魔数）
    int my_mutex = 0;
    sched_tag(UNSHARED, SCHED_UNSHARED_HELD, &my_mutex);
    critical_logic_with_lock();
    sched_tag(UNSHARED, SCHED_UNSHARED_NONE); // 离开时置 NONE 即自动清空魔数

    // 3. 跨进程共调度（直接传递一个静态的 64 位整型 ID 作为魔数）
    sched_tag(UNSHARED, SCHED_UNSHARED_HELD, 0xDEADBEEF12345678ULL);
    // ...
    sched_tag(UNSHARED, SCHED_UNSHARED_NONE);
}
```

> header-only 方式同样依赖 `linux/sched_hint.h`（`sched_tag.h` 会包含它）。头文件不在默认 sysroot 时，编译加 `-I /path/to/kernel/usr/include`。

### 方式二：基于 LLVM Pass 的自动化插桩

使用 `clang` 编译时，通过 `-fpass-plugin` 加载编译出的 Pass 插件即可对 C/C++ 项目进行无侵入式的标签插桩。默认情况下，Pass 会读取当前目录下的 `sched_tags.json` 文件。

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

### 1. 硬件资源需求

| 标签类型     | 使用场景                                             | 值                        | 调度目标                                                                       |
| ------------ | ---------------------------------------------------- | ------------------------- | ------------------------------------------------------------------------------ |
| `exec-dense` | 执行资源压力大的代码（整数/浮点/SIMD 运算、分支密集） | INT/FLOAT/SIMD/CTRL（或按位或组合，如 INT\|CTRL） | SMT 配对：执行资源掩码不相交的任务适合作同核兄弟线程（如 SIMD 配分支密集） |
| `memory-dense` | 内存访问密集代码（大量 load/store）                | STREAM/RANDOM             | STREAM 顺序扫描迁移代价低、适合作计算任务的兄弟线程；RANDOM 指针追逐避免同类配对 |

### 2. 线程同步依赖

| 标签类型      | 使用场景                                 | 值                              | 调度目标                                                                              |
| ------------- | ---------------------------------------- | ------------------------------- | ------------------------------------------------------------------------------------- |
| `atomic-dense` | 原子操作密集且计算逻辑极简（CAS 循环、自旋锁） | 1（配合 `magic_vars` 或静态魔数） | 将访问相同原子变量的线程调度到相同核心或 SMT 核，避免 MESI 协议在原子指令中冲刷缓存（用并发度换缓存命中率） |
| `unshared`    | 独占资源被占用时（锁保护的临界区）       | 1（ranged start/end + magic）   | 持锁者被抢占时若有等待者则扩展时间片，防止优先级反转                                    |
| `dependency`  | IPC 通信中保存魔数                       | 1（魔数）                       | 将魔数赋值给标签，相同依赖的代码块会有相同魔数                                          |

### 3. 负载预告

| 标签类型     | 使用场景                     | 值                | 调度目标                                       |
| ------------ | ---------------------------- | ----------------- | ---------------------------------------------- |
| `load-trend` | 负载即将上升 / 即将收尾空闲  | RISING / FALLING  | RISING 主动抬升 CPU 频率；FALLING 提前降频、进入深 C-state，不依赖负载检测 |

## SchedQL 查询语言

定义见[schedql.ebnf](./schedql.ebnf)

## TODO List

- [ ] `loop[in=Type]` 模式
- [ ] `loop[not_in=Type]` 模式
- [x] `func=name` 谓词（调用特定函数）
- [x] `var=name` 谓词（使用特定变量）
- [ ] 函数签名精确匹配
- [ ] `sched_tags.json` 中的 `files` 字段支持使用正则表达式匹配文件名或路径
- [ ] 实现基于后向支配树（Post-Dominator Tree）的模块级控制流分析，确保 `unshared` 等标签的 `start` 和 `end` 范围完全封闭，避免提前 `return` 或异常导致标签逃逸。
- [ ] 处理异常展开（Exception Unwinding）时的标签清理（TLS 清空）。
