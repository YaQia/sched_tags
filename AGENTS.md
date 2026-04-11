# 任务：生成调度标签配置文件 (sched_tags.json)

请分析项目仓库中的源代码，根据代码特征在项目根目录生成 `sched_tags.json` 配置文件，用于 LLVM 调度优化 Pass。

## 输出格式

生成 JSON 文件，格式如下：

```json
{
    "labels": [
        {
            "type": "<label-type>",
            "files": "<file-name-or-folder>" | ["<file1>", "<file2>"],
            "query": "<schedql-query>" | { "start": "<schedql-query>", "end": "<schedql-query>" },
            "value": "<label-value>",
            "magic_vars": ["<var-name1>", "<var-name2>"],
            "comment": "<human-readable explanation>"
        }
    ]
}
```

### 字段说明

| 字段         | 必需 | 描述                                                                                                                  |
| ------------ | ---- | --------------------------------------------------------------------------------------------------------------------- |
| `type`       | 是   | 标签类型                                                                                                              |
| `files`      | 否   | 指定生效的文件名或目录（支持单个字符串或字符串数组）。如果不指定，则默认对所有文件生效。                              |
| `query`      | 是   | SchedQL 查询语句，支持字符串或对象形式（见下文）                                                                      |
| `value`      | 否   | 标签值（默认 1）。对于 `unshared` 和 `atomic-dense`，如果提供整数，将作为跨进程唯一的静态魔数 (Static Magic Number)。 |
| `magic_vars` | 否   | 变量名数组，用于 bloom filter 计算（适用于 `atomic-dense` 和 `unshared`，与静态魔数互斥）                             |
| `comment`    | 否   | 人类可读的说明                                                                                                        |

### query 字段格式

`query` 字段支持两种形式：

**1. 字符串形式**（适用于大多数标签）：

```json
"query": "@function_name/loop[contains=fmul]/fmul[first]"
```

**2. 对象形式**（支持精确范围控制）：

```json
"query": {
    "start": "<start-position-query>",
    "end": "<end-position-query>"
}
```

**行为规则**：
| 标签类型 | query 形式 | 行为 |
|---------|-----------|------|
| 普通标签 | 字符串 | 在该位置插桩，标签持续到任务 quiescent |
| 普通标签 | 对象 `{ start, end? }` | 只采纳 `start` 位置插桩，`end` 被忽略 |
| `unshared` | 对象 `{ start, end }` | **必须**同时指定 start 和 end，精确控制范围 |
| `unshared` | 字符串或缺少 `end` | **编译报错** |

### unshared 标签的特殊要求

`unshared` 标签用于标记独占资源的持有期间，调度器强依赖其精确范围：

**1. 必须使用对象形式的 query**，同时指定 `start` 和 `end`

**2. 插桩时序要求**：

```
                    ┌─ start 标签必须插在此处（获取资源之前）
                    ▼
线程代码:    [...] → lock(mutex) → [critical section] → unlock(mutex) → [...]
                                                        ▲
                                                        └─ end 标签插在此处（释放资源之后）
```

- `start` 必须在**获取独占资源之前**插桩
  - 原因：调度器需要在任务因等待资源而睡眠时，通过 `unshared=1` 判断睡眠原因
  - 如果 start 插在 lock 之后，任务阻塞时 `unshared=0`，调度器无法感知
- `end` 必须在**释放独占资源之后**插桩
  - 原因：确保资源已完全释放，调度器可以安全地调度其他等待该资源的任务

**3. 错误示例**：

```json
// 错误：unshared 必须使用对象形式
{ "type": "unshared", "query": "@lock/call[first]" }

// 错误：unshared 必须同时有 start 和 end
{ "type": "unshared", "query": { "start": "@lock/call[first]" } }
```

**4. 正确示例**：

```json
{
  "type": "unshared",
  "query": {
    "start": "@pthread_mutex_lock/call[first]",
    "end": "@pthread_mutex_unlock/call[last]"
  },
  "magic_vars": ["mutex"],
  "comment": "Mutex critical section - precise range required for scheduler"
}
```

### magic_vars 字段

对于需要 bloom filter 的标签类型（`atomic-dense`、`unshared`），`magic_vars` 指定哪些变量的地址应被追踪：

- 如果指定了 `magic_vars`，Pass 会在匹配区域内查找这些变量名对应的 SSA 值
- 如果未指定，Pass 会自动检测区域内的原子操作（`atomicrmw`/`cmpxchg`）并提取其指针
- 支持的变量来源：函数参数、全局变量、局部指令（如 `alloca`）

> **跨进程同步的注意事项：**
> 如果遇到跨进程锁（如 System V 信号量、文件锁、跨进程 Mutex），使用 `magic_vars` 获取虚拟地址在不同进程间会失效。此时**请勿使用 `magic_vars`**，而是在 `value` 字段直接填入一个全局唯一的 64 位整数作为静态魔数（Static Magic Number），Pass 将跳过 Bloom Filter 计算，直接使用该常量标识这个独占资源。

## 支持的标签类型

### 1. 指令特性标签

| 标签类型        | 使用场景                                             | 值                                       | 调度目标                                                                                                            |
| --------------- | ---------------------------------------------------- | ---------------------------------------- | ------------------------------------------------------------------------------------------------------------------- |
| `compute-dense` | 计算密集代码片段（大量整数/浮点/SIMD运算）           | INT/FLOAT/SIMD（或者按位或将这些值合并） | 选择计算资源空闲的核心，优化计算吞吐                                                                                |
| `branch-dense`  | 分支密集代码（大量条件跳转）                         | 1                                        | 防止多个分支密集任务在同核心的 SMT 上运行                                                                           |
| `memory-dense`  | 内存访问密集代码（大量 load/store）                  | STREAM/RANDOM                            | 优化 CPU 亲和性，保证在缓存热的核心上运行                                                                           |
| `atomic-dense`  | 原子操作密集且计算逻辑极简的代码（CAS 循环、自旋锁） | 1                                        | 将访问相同原子变量的线程调度到相同核心或SMT核，避免MESI协议在原子指令中冲刷缓存带来的开销（用并发度换取缓存命中率） |

### 2. 资源需求标签

| 标签类型   | 使用场景         | 值  | 调度目标                                                 |
| ---------- | ---------------- | --- | -------------------------------------------------------- |
| `io-dense` | I/O 密集代码片段 | 1   | 保证任务以较高优先级被调度，但忽略 CPU 亲和性            |
| `unshared` | 独占资源被占用时 | 1   | 在拥有该标记时禁止抢占，防止锁持有者被抢占导致系统级阻塞 |

### 3. 行为预测标签

| 标签类型       | 使用场景           | 值  | 调度目标                                       |
| -------------- | ------------------ | --- | ---------------------------------------------- |
| `compute-prep` | 负载即将快速上升   | 1   | 主动抬升 CPU 频率，不依赖负载检测升频          |
| `dependency`   | IPC 通信中保存魔数 | 1   | 将魔数赋值给标签，相同依赖的代码块会有相同魔数 |

## SchedQL 查询语法

### 语法规范 (EBNF)

```ebnf
(* 顶层查询 *)
Query ::= "@" FunctionSpec "/" Target
(* 函数定位 *)
FunctionSpec ::= Identifier
              | Identifier "[" Signature "]"
Signature ::= ArgType ("," ArgType)*
ArgType ::= "i8"
         | "i1"
         | "i16"
         | "i32"
         | "i64"
         | "i128"
         | "u8"
         | "u16"
         | "u32"
         | "u64"
         | "u128"
         | "half"
         | "float"
         | "double"
         | "fp128"
         | "ptr"
         | "[" Number "x" ArgType "]"       (* 数组类型 *)
         | "{" ArgType ("," ArgType)* "}"   (* 结构体 *)
         | "<" Number "x" ArgType ">"       (* 向量类型 *)
(* 目标定位 *)
Target ::= LoopQuery
        | BasicBlockQuery
        | InstructionQuery
(* 循环查询 *)
LoopQuery ::= "loop" "[" PatternList "]"
(* 基本块查询 - 与 LoopQuery 对齐的统一语法 *)
BasicBlockQuery ::= "bb" "[" BBPatternList "]"
BBPatternList ::= BBPattern (";" BBPattern)*
BBPattern ::= "entry"                      (* 入口块 *)
           | "exit"                        (* 退出块，含 ret 指令 *)
           | "name" "=" Identifier         (* 按标签名匹配 *)
           | BBCategory "=" TypeSpec       (* 按指令类型匹配 *)
BBCategory ::= "contains"                  (* 包含该类型指令 *)
            | "in"                         (* 被该类型指令支配 *)
            | "not_in"                     (* 不被该类型指令支配 *)
(* 指令查询 *)
InstructionQuery ::= InstructionType "[" PredicateList "]"
(* Pattern列表 *)
PatternList ::= Pattern (";" Pattern)*
(* Pattern定义 *)
Pattern ::= Category "=" TypeSpec
Category ::= "contains"                 (* 循环内包含 *)
          | "in"                        (* 被...支配 *)
          | "not_in"                    (* 不被...支配 *)
TypeSpec ::= Type
          | Type ":" Identifier
(* 类型定义 *)
Type ::= InstructionType | StructureType
InstructionType ::= "atomicrmw"         (* 原子读-改-写 *)
                 | "cmpxchg"            (* 原子比较交换 *)
                 | "call"               (* 函数调用 *)
                 | "load"               (* 内存读取 *)
                 | "store"              (* 内存写入 *)
                 | "alloca"             (* 栈分配 *)
                 | "br"                 (* 分支 *)
                 | "switch"             (* Switch *)
                 | "ret"                (* 返回 *)
                 | "add" | "fadd"       (* 加法 *)
                 | "mul" | "fmul"       (* 乘法 *)
                 | "sub" | "fsub"       (* 减法 *)
                 | "div" | "fdiv"       (* 除法 *)
                 (* 可扩展其他LLVM指令类型 *)
StructureType ::= "loop"                (* 循环 *)
               | "br"                   (* 条件分支 *)
               | "switch"               (* Switch语句 *)
(* Predicate列表 *)
PredicateList ::= Predicate ("," Predicate)*
Predicate ::= Position
           | "func" "=" Identifier      (* 函数名匹配 *)
           | "var" "=" Identifier       (* 变量名匹配 *)
Position ::= "first"                    (* 第一个匹配 *)
          | "last"                      (* 最后一个匹配 *)
(* 基础类型 *)
Identifier ::= [a-zA-Z_][a-zA-Z0-9_]*
Number ::= [0-9]+
```

### 常见模式

1. **定位循环中的原子操作**（用于自旋锁、无锁数据结构）：

```
@lockfree::Stack::push/loop[contains=cmpxchg]
```

2. **定位循环中的计算密集操作**：

```
@matrix_multiply/loop[contains=fmul]
```

3. **定位函数入口块**：

```
@critical_function/bb[entry]
```

4. **定位特定名称的基本块**：

```
@my_function/bb[name=error_handler]
```

5. **定位包含特定指令的基本块**：

```
@my_function/bb[contains=atomicrmw]
```

6. **组合多个 BB 模式**（entry 块且包含 call 指令）：

```
@my_function/bb[entry;contains=call]
```

7. **定位特定函数调用**：

```
@worker_thread/call[func=pthread_mutex_lock]
```

### 支持的指令类型

- 原子操作：`cmpxchg`, `atomicrmw`
- 内存操作：`load`, `store`
- 算术操作：`add`, `fadd`, `mul`, `fmul`, `div`, `fdiv`
- 控制流：`br`, `call`, `ret`

### 支持的谓词

- `[first]` - 第一个匹配的指令
- `[last]` - 最后一个匹配的指令
- `[entry]` - 入口位置
- `[contains=Type]` - 循环包含特定类型指令

## 生成规则

1. **识别性能关键路径**：
   - 无锁数据结构的 CAS 循环 → `atomic-dense`
   - 自旋锁实现 → `atomic-dense`
   - 紧密的计算循环（矩阵运算、图像处理）→ `compute-dense`
   - 大量条件分支的代码 → `branch-dense`
2. **每个标签必须包含**：
   - `type`：标签类型
   - `query`：精确的 SchedQL 查询
   - `comment`：清晰的中文或英文说明
3. **优先级**：
   - 首先标记原子操作热点（最高优先级）
   - 其次标记计算密集循环
   - 最后标记其他性能关键路径

## 示例输出

```json
{
  "labels": [
    {
      "type": "atomic-dense",
      "query": "@lockfree::Stack::push/loop[contains=cmpxchg]/cmpxchg[first]",
      "value": 1,
      "comment": "Lock-free stack push - CAS retry loop for head pointer update"
    },
    {
      "type": "compute-dense",
      "files": "image_processing.c",
      "query": "@image::blur_kernel/loop[contains=fmul]/fmul[first]",
      "value": "INT|FLOAT",
      "comment": "Image blur kernel - intensive floating-point convolution loop"
    },
    {
      "type": "memory-dense",
      "files": ["database.cpp", "scan/"],
      "query": "@database::scan_records/loop[contains=load]/load[first]",
      "value": "STREAM",
      "comment": "Database table scan - sequential memory access pattern"
    },
    {
      "type": "unshared",
      "query": {
        "start": "@worker_thread/call[func=pthread_mutex_lock]",
        "end": "@worker_thread/call[func=pthread_mutex_unlock]"
      },
      "magic_vars": ["task_queue_mutex"],
      "comment": "Task queue mutex - scheduler needs precise hold duration"
    }
  ]
}
```
