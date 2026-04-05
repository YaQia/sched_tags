# 任务：生成调度标签配置文件 (sched_tags.json)
请分析项目仓库中的源代码，根据代码特征在项目根目录生成 `sched_tags.json` 配置文件，用于 LLVM 调度优化 Pass。
## 输出格式
生成 JSON 文件，格式如下：
```json
{
    "labels": [
        {
            "type": "<label-type>",
            "query": "<schedql-query>",
            "value": "<label-value>"
            "comment": "<human-readable explanation>"
        }
    ]
}
```
## 支持的标签类型
### 1. 指令特性标签
| 标签类型 | 使用场景 | 值 | 调度目标 |
|----------|----------|----|-------|
| `compute-dense` | 计算密集代码片段（大量整数/浮点/SIMD运算） | INT/FLOAT/SIMD（或者按位或将这些值合并） | 选择计算资源空闲的核心，优化计算吞吐 |
| `branch-dense` | 分支密集代码（大量条件跳转） | 1 | 防止多个分支密集任务在同核心的 SMT 上运行 |
| `memory-dense` | 内存访问密集代码（大量 load/store） | STREAM/RANDOM | 优化 CPU 亲和性，保证在缓存热的核心上运行 |
| `atomic-dense` | 原子操作密集且计算逻辑极简的代码（CAS 循环、自旋锁） | 1 | 将访问相同原子变量的线程调度到相同核心或SMT核，避免MESI协议在原子指令中冲刷缓存带来的开销（用并发度换取缓存命中率） |
### 2. 资源需求标签
| 标签类型 | 使用场景 | 调度目标 |
|---------|---------|---------|
| `io-dense` | I/O 密集代码片段 | 1 | 保证任务以较高优先级被调度，但忽略 CPU 亲和性 |
| `unshared` | 独占资源被占用时 | 1 | 在拥有该标记时禁止抢占，防止优先级反转 |
### 3. 行为预测标签
| 标签类型 | 使用场景 | 调度目标 |
|---------|---------|---------|
| `compute-prep` | 负载即将快速上升 | 1 | 主动抬升 CPU 频率，不依赖负载检测升频 |
| `dependency` | IPC 通信中保存魔数 | 1 | 将魔数赋值给标签，相同依赖的代码块会有相同魔数 |
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
(* 基本块查询 *)
BasicBlockQuery ::= "bb" BlockSpec
BlockSpec ::=  Identifier                 (* 标签名 *)
           |   "entry"                    (* 入口块 *)
           |   "exit"                     (* 退出块 *)
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
          | "entry"                     (* 入口指令 *)
(* 基础类型 *)
Identifier ::= [a-zA-Z_][a-zA-Z0-9_]*
Number ::= [0-9]+
```
### 常见模式
1. **定位循环中的原子操作**（用于自旋锁、无锁数据结构）：
```
@lockfree::Stack::push/loop[contains=cmpxchg]/cmpxchg[first]
```
2. **定位循环中的计算密集操作**：
```
@matrix_multiply/loop[contains=fmul]/fmul[first]
```
3. **定位函数入口**：
```
@critical_function/bb entry/call[first]
```
4. **定位特定基本块中的指令**：
```
@my_function/bb block_name/load[first]
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
            "query": "@image::blur_kernel/loop[contains=fmul]/fmul[first]",
            "value": "INT|FLOAT",
            "comment": "Image blur kernel - intensive floating-point convolution loop"
        },
        {
            "type": "memory-dense",
            "query": "@database::scan_records/loop[contains=load]/load[first]",
            "value": "STREAM",
            "comment": "Database table scan - sequential memory access pattern"
        }
    ]
}
```
