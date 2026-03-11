# test/ — 运行时验证测试

本目录包含用于验证 `SchedTagPass` LLVM pass 插桩正确性的端到端测试。

## 测试原理

核心思路是**在密集区域执行期间**观测 hint 状态，而不仅仅在函数返回后检查。

`.ll` 文件中的密集 BB / 循环体内嵌入了 `call void @observe_hint(i32 %id)`，
该函数在 `reader*.c` 中实现，会在被调用时快照 `__sched_hint_data` 的当前状态。
这样 reader 的 `main` 函数就能同时验证两件事：

1. **SET 验证** — 回调捕获的快照中 `tags_active` 和 `compute_dense` 是否正确
2. **CLR 验证** — 函数返回后 hint 是否被清零

## 文件说明

```
test/
├── test_dense.ll       # LLVM IR 输入: BB 级别密集区域 (INT / FLOAT / SIMD)
├── test_loops.ll       # LLVM IR 输入: 循环级别密集区域 + BB 回退
├── reader.c            # C 运行时验证器: 配合 test_dense.ll
└── reader_loops.c      # C 运行时验证器: 配合 test_loops.ll
```

### `.ll` 文件 — 测试输入

手写的 LLVM IR，每个函数构造了特定的指令分布比例以触发 pass 的密度分析。
密集区域中间插入了 `call void @observe_hint(i32 %id)`，`call` 指令在 pass 的
`computeOpType` 中归类为 NONE，因此每个区域的指令数需要适当增加以保证加入
call 后密度比仍然超过阈值。

| 文件 | 函数 | 观测点 ID | 测试目标 |
|------|------|----------|---------|
| `test_dense.ll` | `@workload` int_work 分支 | 1 | BB 级别 INT SET |
| `test_dense.ll` | `@workload` float_work 分支 | 2 | BB 级别 FLOAT SET |
| `test_dense.ll` | `@simd_kernel` | (无) | SIMD 分析（无回调，参数是向量类型） |
| `test_dense.ll` | `@trivial` | (无) | 不应被插桩 |
| `test_loops.ll` | `@int_loop_dense` | 10 | 循环级别 INT SET（每次迭代回调） |
| `test_loops.ll` | `@float_loop_dense` | 20 | 循环级别 FLOAT SET（每次迭代回调） |
| `test_loops.ll` | `@simd_loop_dense` | (无) | SIMD 循环分析（无回调） |
| `test_loops.ll` | `@mixed_with_dense_bb` | 30 | BB 回退 INT SET |
| `test_loops.ll` | `@trivial` | (无) | 不应被插桩 |

### `reader*.c` — 运行时验证器

每个 reader 文件包含：

- **`observe_hint(int tag_id)`** — 回调函数，读取 TLS 变量 `__sched_hint_data`
  的 `tags_active` 和 `compute_dense`，保存为快照
- **`check()`** — 断言辅助，比较期望值与实际值，统计失败数
- **`main()`** — 依次调用被插桩函数，检查回调快照（SET 验证）和返回后状态（CLR 验证），
  最终以退出码反映测试结果（0 = 全部通过）

## 前置条件

- 已构建 pass 插件: `build/pass/libSchedTagPass.so`（项目根目录执行 `cmake --build build`）
- 系统安装了 `opt`、`llc`、`clang`（版本需与构建 pass 时使用的 LLVM 一致）

## 运行方法

### 测试 1: BB 级别插桩

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

### 测试 2: 循环级别插桩

```bash
opt -load-pass-plugin=build/pass/libSchedTagPass.so \
    -passes=sched-tag test/test_loops.ll -o /tmp/tagged_loops.bc

llc /tmp/tagged_loops.bc -filetype=obj -o /tmp/tagged_loops.o

clang /tmp/tagged_loops.o test/reader_loops.c -lm -o /tmp/test_loops

/tmp/test_loops
```

## 流水线图解

```
  .ll (手写 IR, 密集区域内含 observe_hint 回调)
       │
       ▼
  ┌──────────┐
  │   opt    │  加载 libSchedTagPass.so, 运行 -passes=sched-tag
  └────┬─────┘  → 在密集区域边界插入 SET/CLR store 指令
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

### test_reader (BB 级别)

```
=== struct sched_hint ===
magic:        0x5348494e (OK)
version:      1
tags_present: 0x1
sizeof:       64 bytes

  [initial state               ] tags_active=0x0  compute_dense=NONE   OK

--- workload(20, 3.14) [int_work path] ---
  result = 8880
  [inside int_work (cb)        ] tags_active=0x1  compute_dense=INT    OK
  [after return (main)         ] tags_active=0x0  compute_dense=NONE   OK

--- workload(5, 2.71) [float_work path] ---
  result = 22
  [inside float_work (cb)      ] tags_active=0x1  compute_dense=FLOAT  OK
  [after return (main)         ] tags_active=0x0  compute_dense=NONE   OK

--- trivial(42) [no dense BBs] ---
  result = 43
  [after trivial (main)        ] tags_active=0x0  compute_dense=NONE   OK

=== ALL PASSED (0 failure(s)) ===
```

### test_loops (循环级别)

```
=== struct sched_hint ===
magic:        0x5348494e (OK)
...
  [inside int loop (cb)              ] tags_active=0x1  compute_dense=INT    OK
  [after int_loop_dense (main)       ] tags_active=0x0  compute_dense=NONE   OK
  [inside float loop (cb)            ] tags_active=0x1  compute_dense=FLOAT  OK
  [after float_loop_dense (main)     ] tags_active=0x0  compute_dense=NONE   OK
  [inside dense_bb (cb)              ] tags_active=0x1  compute_dense=INT    OK
  [after mixed_dense_bb(200) (main)  ] tags_active=0x0  compute_dense=NONE   OK
...
=== ALL PASSED (0 failure(s)) ===
```

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
