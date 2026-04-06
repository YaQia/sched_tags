# 标签化调度优化器

## 编译

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

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

```ebnf

(* SchedQL - 调度标签查询语言 *)

(* 顶层查询 *)
    Query ::= "@" FunctionSpec "/" Target

(* 函数定位 *)
    FunctionSpec ::= Identifier | Identifier "[" Signature "]"
    Signature ::= Type ("," Type)*

    (* 目标定位 *)
    (* 三种互斥的查询类型，各有固定的插桩位置 *)
    Target ::= LoopQuery           (* 插桩位置: loop preheader *)
    | BasicBlockQuery      (* 插桩位置: BB 入口 (first non-PHI) *)
| InstructionQuery     (* 插桩位置: 包含该指令的 BB 入口 *)

    (* 循环查询 - 匹配包含指定模式的循环 *)
(* Base pointers 自动从循环中所有 AtomicRMW/CmpXchg 指令收集 *)
    LoopQuery ::= "loop" "[" PatternList "]"

(* 基本块查询 - 匹配指定的基本块 *)
    BasicBlockQuery ::= "bb" "[" BlockSpec "]"
    BlockSpec ::= Identifier | "entry" | "exit"

    (* 指令查询 - 匹配包含指定指令的 BB *)
(* 用于 BB 级标签：在函数中搜索包含该指令的所有 BB *)
    InstructionQuery ::= InstructionType "[" PredicateList "]"

(* Pattern 定义 - 用于循环匹配 *)
    PatternList ::= Pattern (";" Pattern)*
    Pattern ::= Category "=" TypeSpec
    Category ::= "contains" | "in" | "not_in"
    TypeSpec ::= Type | Type ":" Identifier

(* 类型定义 *)
    Type ::= InstructionType | StructureType
    InstructionType ::= "atomicrmw" | "cmpxchg" | "call" | "load" | "store"
    | "alloca" | "br" | "switch" | "ret" | "add" | "fadd"
    | "mul" | "fmul" | "sub" | "fsub" | "div" | "fdiv"
    StructureType ::= "loop" | "br" | "switch"

(* Predicate 定义 - 用于指令过滤 *)
    PredicateList ::= Predicate ("," Predicate)*
    Predicate ::= Position | "func" "=" Identifier | "var" "=" Identifier
    Position ::= "first" | "last" | "entry"

(* 基础类型 *)
    Identifier ::= [a-zA-Z_:][a-zA-Z0-9_:]*   (* 支持 C++ 命名空间如 Foo::Bar *)
    Number ::= [0-9]+

    (* 示例查询 *)
    (* @Disruptor::Sequence::compareAndSet/cmpxchg[first]     - BB级: 匹配含cmpxchg的BB *)
    (* @Disruptor::SequenceGroups::addSequences/loop[contains=call]  - 循环级: 匹配含call的循环 *)
(* @Disruptor::SpinWait::spinOnce/bb[entry]               - BB级: 匹配函数入口块 *)
```

### TODO List

- [ ] `loop[in=Type]` 模式
- [ ] `loop[not_in=Type]` 模式
- [ ] `func=name` 谓词（调用特定函数）
- [ ] `var=name` 谓词（使用特定变量）
- [ ] 函数签名精确匹配
