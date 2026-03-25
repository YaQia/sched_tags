# test/ — 运行时验证测试

本目录包含用于验证 `SchedTagPass` LLVM pass 插桩正确性的端到端测试。

## TLS 变量访问说明

**重要变更**：从现在开始，编译器插桩生成的TLS变量符号名为 `__sched_hint.data`（包含句点），
这在C/C++源码中是非法标识符，**无法通过常规的 `extern __thread` 声明直接访问**。

### 为什么使用特殊字符？

1. **防止用户干扰**：用户代码无法定义同名变量来干扰编译器插桩
2. **保持链接一致性**：固定名称确保多个 `.o` 文件链接时符号一致
3. **源码层面隔离**：句点 `.` 在 C/C++ 标识符中非法

### 如何访问？

使用 `test/reader_dynamic.h` 提供的辅助函数：

```c
#include "reader_dynamic.h"

struct sched_hint *hint = get_sched_hint_data();
// 现在可以使用 hint->compute_dense 等字段
```

该头文件使用内联汇编（通过 `__asm__` 重命名）访问符号。

## 测试原理

核心思路是**在密集区域执行期间**观测 hint 状态。

`.ll` 文件中的密集 BB / 循环体内嵌入了 `call void @observe_hint(i32 %id)`，
该函数在 `reader*.c` 中实现，会在被调用时快照 TLS 变量的当前状态。
这样 reader 的 `main` 函数就能验证：

1. **SET 验证** — 回调捕获的快照中 payload 字段（及 `atomic_magic` bloom filter）是否正确

**注意**：标签清除现在由内核调度器在任务上下文切换时处理，不再由编译器插桩实现。
因此测试不再验证函数返回后的标签清除行为。`atomic_magic` 永不清除（性能优化），
仅在 `atomic_dense=1` 时有效。

## 文件说明

```
test/
├── test_dense.ll           # LLVM IR 输入: BB 级别计算密集区域 (INT / FLOAT / SIMD)
├── test_loops.ll           # LLVM IR 输入: 循环级别计算密集区域 + BB 回退
├── test_atomic.ll          # LLVM IR 输入: 原子指令密集区域 (BB / loop / mixed)
├── test_rust_dense.rs      # Rust 源码: 计算密集测试 (编译为 .ll 后插桩)
├── reader.c                # C 运行时验证器: 配合 test_dense.ll（使用旧符号名）
├── reader_loops.c          # C 运行时验证器: 配合 test_loops.ll
├── reader_atomic.c         # C 运行时验证器: 配合 test_atomic.ll
├── reader_rust.c           # C 运行时验证器: 配合 Rust 生成的 IR
├── reader_prctl.c          # prctl 构造函数测试
├── reader_obfuscated.c     # 使用动态符号查找的测试（推荐）
└── reader_dynamic.h        # TLS 变量访问辅助函数
```

### `.ll` 文件 — 测试输入

手写的 LLVM IR，每个函数构造了特定的指令分布比例以触发 pass 的密度分析。
密集区域中间插入了 `call void @observe_hint(i32 %id)`，`call` 指令在 pass 的
分析中归类为 NONE，因此每个区域的指令数需要适当增加以保证加入
call 后密度比仍然超过阈值。

#### 计算密集 (compute_dense)

| 文件            | 函数                        | 观测点 ID | 测试目标                            |
| --------------- | --------------------------- | --------- | ----------------------------------- |
| `test_dense.ll` | `@workload` int_work 分支   | 1         | BB 级别 INT SET                     |
| `test_dense.ll` | `@workload` float_work 分支 | 2         | BB 级别 FLOAT SET                   |
| `test_dense.ll` | `@simd_kernel`              | (无)      | SIMD 分析（无回调，参数是向量类型） |
| `test_dense.ll` | `@trivial`                  | (无)      | 不应被插桩                          |
| `test_loops.ll` | `@int_loop_dense`           | 10        | 循环级别 INT SET（每次迭代回调）    |
| `test_loops.ll` | `@float_loop_dense`         | 20        | 循环级别 FLOAT SET（每次迭代回调）  |
| `test_loops.ll` | `@simd_loop_dense`          | (无)      | SIMD 循环分析（无回调）             |
| `test_loops.ll` | `@mixed_with_dense_bb`      | 30        | BB 回退 INT SET                     |
| `test_loops.ll` | `@trivial`                  | (无)      | 不应被插桩                          |

#### 原子指令密集 (atomic_dense)

| 文件             | 函数                | 观测点 ID | 测试目标                           |
| ---------------- | ------------------- | --------- | ---------------------------------- |
| `test_atomic.ll` | `@atomic_bb_work`   | 1         | BB 级别原子密集 SET (8 atomicrmw)  |
| `test_atomic.ll` | `@atomic_loop_work` | 10        | 循环级别原子密集 SET (6 atomicrmw) |
| `test_atomic.ll` | `@mixed_atomic_bb`  | 30        | 非密集循环 + 独立原子密集 BB       |
| `test_atomic.ll` | `@trivial`          | (无)      | 无原子操作 → 不应被插桩            |

### `reader*.c` — 运行时验证器

每个 reader 文件包含：

- **`observe_hint(int tag_id)`** — 回调函数，读取 TLS 变量 `__sched_hint_data`
  的 payload 字段（含 `atomic_magic` bloom filter），保存为快照
- **`check()`** — 断言辅助，比较期望值与实际值，统计失败数
- **`main()`** — 依次调用被插桩函数，检查回调快照（SET 验证），
  最终以退出码反映测试结果（0 = 全部通过）

**注意**：由于标签清除现在由内核调度器处理，测试不再检查函数返回后的标签状态。

## 前置条件

- 已构建 pass 插件: `build/pass/libSchedTagPass.so`（项目根目录执行 `cmake --build build`）
- 系统安装了 `opt`、`llc`、`clang`（版本需与构建 pass 时使用的 LLVM 一致，当前为 **LLVM 22**）

## 运行方法

### 测试 1: BB 级别计算密集插桩

```bash
# 1. 用 pass 插桩
opt -load-pass-plugin=build/pass/libSchedTagPass.so \
    -passes=sched-tag test/test_dense.ll -o /tmp/tagged.bc

# 2. bitcode → 目标文件
llc /tmp/tagged.bc -filetype=obj -o /tmp/tagged.o

# 3. 链接
clang /tmp/tagged.o test/reader.c -lm -o /tmp/test_reader

# 4. 运行（退出码 0 = 通过）
/tmp/test_reader
```

### 测试 2: 循环级别计算密集插桩

```bash
opt -load-pass-plugin=build/pass/libSchedTagPass.so \
    -passes=sched-tag test/test_loops.ll -o /tmp/tagged_loops.bc

llc /tmp/tagged_loops.bc -filetype=obj -o /tmp/tagged_loops.o

clang /tmp/tagged_loops.o test/reader_loops.c -lm -o /tmp/test_loops

/tmp/test_loops
```

### 测试 3: 原子指令密集插桩

```bash
opt -load-pass-plugin=build/pass/libSchedTagPass.so \
    -passes=sched-tag test/test_atomic.ll -o /tmp/tagged_atomic.bc

llc /tmp/tagged_atomic.bc -filetype=obj -o /tmp/tagged_atomic.o

clang /tmp/tagged_atomic.o test/reader_atomic.c -lm -o /tmp/test_atomic

/tmp/test_atomic
```

### 测试 4: Rust 生成的 IR

```bash
# 1. Rust → LLVM IR
rustc --emit=llvm-ir -C opt-level=1 -C overflow-checks=no \
      -C no-prepopulate-passes --crate-type=lib \
      test/test_rust_dense.rs -o /tmp/test_rust_dense.ll

# 2. 优化 + 插桩
opt -load-pass-plugin=build/pass/libSchedTagPass.so \
    -passes='default<O1>,sched-tag' /tmp/test_rust_dense.ll \
    -o /tmp/tagged_rust.bc

# 3. bitcode → 目标文件
llc /tmp/tagged_rust.bc -filetype=obj -o /tmp/tagged_rust.o

# 4. 链接
clang /tmp/tagged_rust.o test/reader_rust.c -lm -o /tmp/test_rust

# 5. 运行
/tmp/test_rust
```

## 流水线图解

```
  .ll (手写 IR, 密集区域内含 observe_hint 回调)
       │
       ▼
  ┌──────────┐
  │   opt    │  加载 libSchedTagPass.so, 运行 -passes=sched-tag
  └────┬─────┘  → 在密集区域入口插入 SET store 指令 (不插入 CLR)
       │ .bc
       ▼
  ┌──────────┐
  │   llc    │  bitcode → 目标文件
  └────┬─────┘
       │ .o (含 __sched_hint TLS 段)
       ▼
  ┌──────────┐
  │  clang   │  链接 reader*.c (提供 observe_hint 实现) + 插桩后的 .o
  └────┬─────┘
       │ 可执行文件
       ▼
  ┌──────────┐
  │   运行   │  observe_hint 在密集区域内捕获 hint 快照
  └──────────┘  main 比较快照与期望值 → 输出 OK/FAIL
```

## 预期输出

### test_reader (BB 级别计算密集)

```
=== struct sched_hint ===
magic:        0x5348494e (OK)
version:      1
sizeof:       64 bytes

  [initial state               ] compute_dense=NONE   OK

--- workload(20, 3.14) [int_work path] ---
  result = 8880
  [inside int_work (cb)        ] compute_dense=INT    OK

--- workload(5, 2.71) [float_work path] ---
  result = 22
  [inside float_work (cb)      ] compute_dense=FLOAT  OK

--- trivial(42) [no dense BBs] ---
  result = 43

=== ALL PASSED (0 failure(s)) ===
```

### test_loops (循环级别计算密集)

```
=== struct sched_hint ===
magic:        0x5348494e (OK)
...
  [inside int loop (cb)              ] compute_dense=INT    OK
  [inside float loop (cb)            ] compute_dense=FLOAT  OK
  [inside dense_bb (cb)              ] compute_dense=INT    OK
...
=== ALL PASSED (0 failure(s)) ===
```

### test_atomic (原子指令密集 + bloom filter)

```
=== Atomic-dense SchedTag test (with bloom filter) ===
magic:        0x5348494e (OK)
version:      1
sizeof:       64 bytes

bloom reference for &scratch (0x...): 0x... (popcount=4)

  [initial state                     ] atomic_dense=0  OK
  [initial state (magic)             ] atomic_magic=0x0000000000000000

--- atomic_bb_work [BB-level atomic dense] ---
  result = ..., observe_count = 1
  [inside atomic_bb (cb)             ] atomic_dense=1  OK
  [inside atomic_bb magic (cb)       ] atomic_magic=0x... (popcount=4)  OK
  [atomic_bb magic vs ref            ] magic=0x... expect=0x...  OK

--- atomic_loop_work(10) [loop-level atomic dense] ---
  result = ..., observe_count = 10
  [inside atomic loop (cb)           ] atomic_dense=1  OK
  [inside atomic loop magic (cb)     ] atomic_magic=0x... (popcount=4)  OK
  [atomic_loop magic vs ref          ] magic=0x... expect=0x...  OK

--- mixed_atomic_bb(200) [atomic BB path] ---
  result = ..., observe_count = 1
  [inside atomic_bb (cb)             ] atomic_dense=1  OK
  [inside mixed_bb magic (cb)        ] atomic_magic=0x... (popcount=4)  OK
  [mixed_bb magic vs ref             ] magic=0x... expect=0x...  OK

--- mixed_atomic_bb(50) [skip atomic BB] ---
  result = ..., observe_count = 0 (expect 0)

--- trivial(42) [no atomic ops] ---
  result = 43, observe_count = 0 (expect 0)

=== ALL PASSED (0 failure(s)) ===
```

> **Note:** The exact `atomic_magic` hex value depends on the runtime address of
> `&scratch` (stack ASLR). The test verifies: (1) non-zero inside dense regions,
> (2) `popcount >= 4` (k=4 bloom bits), (3) matches the C reference
> `bloom_hash_reference(&scratch)`. **atomic_dense** clearing is now handled by
> the kernel scheduler on context switch. `atomic_magic` is never cleared
> (performance optimization); its value is stale when `atomic_dense=0`.

## 调试技巧

查看 pass 的插桩日志（输出到 stderr）：

```bash
opt -load-pass-plugin=build/pass/libSchedTagPass.so \
    -passes=sched-tag test/test_dense.ll -o /dev/null 2>&1
```

查看插桩后的 IR：

```bash
opt -load-pass-plugin=build/pass/libSchedTagPass.so \
    -passes=sched-tag -S test/test_dense.ll 2>/dev/null
```

用 `readelf` 确认 TLS 符号类型和 section：

```bash
readelf -s /tmp/tagged.o | grep sched_hint
readelf -S /tmp/tagged.o | grep sched_hint
```
