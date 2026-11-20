# sched_tag

源码级的调度标签 API + 一个轻量库 `libsched_tag.a`。

## 标签语义

`struct sched_hint` 一共 5 个标签字段，按携带的信息量从少到多分两类：

- **仅带状态的标签**：只有 state byte
- **带附加信息的标签**：state byte + 一个或多个 magic key

### 1. 仅带状态的标签

| 标签           | state 值                                                 | 调度器用途                                                     |
| -------------- | -------------------------------------------------------- | -------------------------------------------------------------- |
| `compute_dense` | `SCHED_COMPUTE_INT` / `FLOAT` / `SIMD` / `CTRL` 的 bitmask | SMT 配对：compute 掩码不相交的任务适合作同核兄弟线程           |

这个标签只写 state byte，没有附加信息；可以用 `sched_tag_enter_compute()` / `sched_tag_leave_compute()` 做作用域嵌套，也可以用 `sched_tag_set()` 直接设置。

### 2. 带附加信息的标签

这些标签同时有 **state byte** 和 **附加 magic key**：state 表示当前是否 active / role，magic 用于标识具体的 lock / 集合 / 交接对象 / 热点区域。

| 标签           | state 值                                            | 附加信息（magic key）         | 写入方式                 | 调度器用途                                  |
| -------------- | --------------------------------------------------- | ----------------------------- | ------------------------ | ------------------------------------------- |
| `memory_dense` | `SCHED_MEMORY_READ_MOSTLY` / `WRITE_HEAVY`          | `local_memory_magic` + `global_memory_magic` | `enter_memory()` / `sched_tag_set()` | 共享热点靠近，写密集时避免跨 LLC |
| `unshared`     | `SCHED_UNSHARED_HELD` / `NONE`                      | 当前唯一 lock 的 key          | 覆盖 + token 恢复        | 按 lock key 统计 waiter，决定是否扩展时间片 |
| `atomic_dense` | `SCHED_ATOMIC_DENSE` / `NONE`                       | 原子变量集合的 exact set hash | XOR 累加 + clear/restore | 原子密集任务按集合 key 共置                 |
| `dep_role`     | `SCHED_DEP_NONE` / `PRODUCER` / `CONSUMER` / `BOTH` | 当前交接对象的 key            | 覆盖 + token 恢复        | 生产者消费者模型的任务按交接对象 key 共置   |

`unshared` / `atomic` / `dep` 的 magic 都是 **exact key**：只有 key 完全相等，才表示同一个 lock / 集合 / 交接对象。`memory_dense` 的两个 magic 语义不同：

- `local_memory_magic`：per-mm 热点页 sketch，用相似度比较（同一个 mm 内才有意义）；
- `global_memory_magic`：跨进程共享对象的 exact identity，用相等比较。

magic 的具体编码由库负责，普通用法不需要自己构造；不同标签的编码和比较方式不同，调度器侧按标签分别处理。

- 嵌套通过各自的 tag token（`sched_tag_compute` / `memory` / `atomic` / `unshared` / `dep`）保存/恢复外层状态

#### `atomic_dense` 的集合语义

- 集合必须**完全相同**才触发共置优化
- 部分重叠（A: `{x}`，B: `{x,y}`）是不同集合 key，不做 overlap 匹配
- 多变量集合用 `enter_atomic()` + `add_atomic()` + `leave_atomic()`

#### `dep_role`

- 值是一个 bitmask：`SCHED_DEP_NONE` / `PRODUCER` / `CONSUMER` / `BOTH`
- `dep_magic` 表示当前生产者/消费者共享的交接对象；pipe、queue、socket、channel、ring buffer 都可以
- 交接对象必须在生产者和消费者两侧具有相同的 key；同进程可用指针，跨进程用静态 id
- 不同交接对象用嵌套/顺序，不塞进多个 key
- 同一个双向交接对象可以同时是 `BOTH`

#### `memory_dense`

- 只在 memory-dense 阶段设置；`SCHED_MEMORY_NONE` 表示当前不在该阶段。
- `READ_MOSTLY`：写可以忽略，副本可以跨 LLC 分散，利用并行读。
- `WRITE_HEAVY`：有实质性的写，优先同核 / SMT，其次同 LLC，避免跨 LLC 的 coherence 开销。
- `local_memory_magic` 只在同一个 mm 内比较；库根据 1~4 个 local 地址生成热点 sketch。
- 跨进程共享用 `global_memory_magic`，由 `sched_tag_global_id(st_dev, st_ino)` 生成。
- 有多个共享对象时，只提供当前阶段最主要、最能区分的那个。

> Hints are sampled at scheduling hooks. A hint is only observed if it is active
> at a hook. Short userspace-only regions may never be observed. Hints are
> advisory and must not be used for correctness.

## 编译

```bash
make
# 或者指定内核 UAPI 头路径
make KERNEL_HEADERS=/path/to/kernel/usr/include
```

产物：`libsched_tag.a`。

## 使用

```c
#include "sched_tag.h"

void process_data(void) {
    struct sched_tag_unshared ut;
    struct sched_tag_memory mt;
    struct sched_tag_atomic at;
    struct sched_tag_dep dt;
    int lock = 0;
    int counter_a = 0, counter_b = 0;
    int chan = 0;
    int local_hot = 0;
    uint64_t shm_id = sched_tag_global_id(1, 1234); /* st_dev, st_ino */
    const void *hot[] = { &local_hot };

    /* unshared：当前 lock，嵌套安全 */
    ut = sched_tag_enter_unshared(&lock);
    critical_section();
    sched_tag_leave_unshared(&ut);

    /* memory：当前热点区域 + 读写模式 */
    mt = sched_tag_enter_memory(SCHED_MEMORY_WRITE_HEAVY,
                                shm_id, hot, 1);
    memory_dense_region();
    sched_tag_leave_memory(&mt);

    /* atomic：当前原子变量集合 key */
    at = sched_tag_enter_atomic();
    sched_tag_add_atomic(&counter_a);
    sched_tag_add_atomic(&counter_b);
    lock_free_region();
    sched_tag_leave_atomic(&at);

    /* dep：当前 pipe + role */
    dt = sched_tag_enter_dep(&pipe, SCHED_DEP_CONSUMER);
    recv(pipe);
    sched_tag_leave_dep(&dt);

    /* 跨进程 / 静态 id */
    ut = sched_tag_enter_unshared_id(0xDEADBEEF12345678ULL);
    critical_section();
    sched_tag_leave_unshared(&ut);
}
```

编译并链接：

```bash
cc -Iinclude -I/path/to/kernel/usr/include your.c libsched_tag.a -o your
```

`unshared` / `atomic` / `dep` 都有 `_id` 变体，用于静态 ID 或跨进程对象。

## 兼容接口 `sched_tag_set()`

`sched_tag_set()` 保留给旧代码和简单场景。它**不保存/恢复外层状态**，所以嵌套区域仍应使用 `enter_*` / `leave_*`。

### 2 / 3 / 4 参数形式

```c
/* 只写 state byte，不带 magic */
sched_tag_set(TYPE, value);

/* 写 state + key（C11 用 _Generic 自动区分指针和整数） */
sched_tag_set(TYPE, value, &object);   /* pointer key */
sched_tag_set(TYPE, value, 0xDEADBEEF); /* static id key */

/* 4 参数：仅 MEMORY_DENSE */
sched_tag_set(MEMORY_DENSE, value, global_id, &local_addr);
```

- unshared / atomic / dep：指针和整数 id 都由库转成 exact key
- memory：指针是 local 热点地址，整数是 `sched_tag_global_id()` 生成的 global_id
- `value = 0` 时，对应 magic 会被清 0
- 4 参数形式只允许 `MEMORY_DENSE`；其他 type 在 C11 下编译期报错

### 每种标签的语义

```c
/* 非作用域标签：只有 state */
sched_tag_set(COMPUTE_DENSE, SCHED_COMPUTE_INT);
sched_tag_set(MEMORY_DENSE, SCHED_MEMORY_READ_MOSTLY);
sched_tag_set(COMPUTE_DENSE, SCHED_COMPUTE_NONE); /* 清空 */

/* memory：local-only / global-only / both */
sched_tag_set(MEMORY_DENSE, SCHED_MEMORY_WRITE_HEAVY, &local_hot);
sched_tag_set(MEMORY_DENSE, SCHED_MEMORY_WRITE_HEAVY, shm_id);
sched_tag_set(MEMORY_DENSE, SCHED_MEMORY_WRITE_HEAVY, shm_id, &local_hot);
sched_tag_set(MEMORY_DENSE, SCHED_MEMORY_NONE); /* state 和两个 magic 一起清 */

/* unshared：当前 lock 的 exact key */
sched_tag_set(UNSHARED, SCHED_UNSHARED_HELD, &lock);
/* ... critical section ... */
sched_tag_set(UNSHARED, SCHED_UNSHARED_NONE); /* state 和 magic 一起清 */

/* 跨进程 lock：静态 id */
sched_tag_set(UNSHARED, SCHED_UNSHARED_HELD, 0xDEADBEEF12345678ULL);
sched_tag_set(UNSHARED, SCHED_UNSHARED_NONE);

/* dep：当前 pipe + role */
sched_tag_set(DEP_ROLE, SCHED_DEP_CONSUMER, &pipe);
sched_tag_set(DEP_ROLE, SCHED_DEP_NONE);

/* atomic：只适合单变量集合 */
sched_tag_set(ATOMIC_DENSE, SCHED_ATOMIC_DENSE, &counter);
sched_tag_set(ATOMIC_DENSE, SCHED_ATOMIC_NONE);
```

多变量 atomic 集合不要连续调用三次 `sched_tag_set(..., &var)`，否则后一个会覆盖前一个；请使用：

```c
struct sched_tag_atomic t = sched_tag_enter_atomic();
sched_tag_add_atomic(&counter_a);
sched_tag_add_atomic(&counter_b);
/* ... */
sched_tag_leave_atomic(&t);
```

C++ 下 `sched_tag_set` 是重载函数；C11 下用 `_Generic` 区分 `&object` 和整数 id。C99 及更早版本没有自动区分，三参数形式会按指针处理；静态 id 请使用 `sched_tag_enter_*_id()` 这类 API。

## 读取

```c
uint8_t state = sched_tag_get(UNSHARED);

/* exact key 比较；只有 key 相等才视为同一个对象 */
bool same = sched_tag_test_magic(UNSHARED, &lock);

/* 多变量 atomic 集合：比较完整 set hash */
bool same_set = sched_tag_test_key(ATOMIC_DENSE, expected_set_hash);
```

## 测试

```bash
make check
# 或指定头路径
make check KERNEL_HEADERS=/path/to/kernel/usr/include
```

`test/reader_tokens.c` 用 fake slot 验证 token 嵌套、exact key、memory locality key、
atomic set hash、dep role 和兼容 setter，不需要支持 `PR_SET_SCHED_HINT` 的内核。

## 分支

- `master`：源码 API + 库 + token API 测试
- `deprecated/pass`：旧的插桩工具链 / SchedQL / JSON 快照
