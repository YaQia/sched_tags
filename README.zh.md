# sched_tag

源码级的调度标签 API + 一个轻量库 `libsched_tag.a`。

## 标签语义

`struct sched_hint` 一共 6 个标签字段，按携带的信息量从少到多分两类：

- **仅带状态的标签**：只有 state byte
- **带附加信息的标签**：state byte + exact 64-bit magic key

### 1. 仅带状态的标签

| 标签           | state 值                                                 | 调度器用途                                                     |
| -------------- | -------------------------------------------------------- | -------------------------------------------------------------- |
| `funit_dense`  | `SCHED_FUNIT_INT` / `FLOAT` / `SIMD` / `CTRL` 的 bitmask | SMT 配对：功能单元掩码不相交的任务适合作同核兄弟线程           |
| `memory_dense` | `SCHED_MEMORY_STREAM` / `SCHED_MEMORY_RANDOM`            | STREAM 适合顺序扫描；RANDOM 避免同类配对，减少 cache/NUMA 冲突 |
| `load_ahead`   | `SCHED_LOAD_RISING` / `SCHED_LOAD_FALLING`               | 负载预告：RISING 提前升频，FALLING 提前降频/进入深 C-state     |

这三个标签只写 state byte，没有附加信息；用 `sched_tag_set()` 或对应的状态写入即可。

### 2. 带附加信息的标签

这些标签同时有 **state byte** 和 **附加 magic key**：state 表示当前是否 active / role，magic 用于标识具体的 lock / 集合 / 交接对象。

| 标签           | state 值                                            | 附加信息（magic key）         | 写入方式                 | 调度器用途                                  |
| -------------- | --------------------------------------------------- | ----------------------------- | ------------------------ | ------------------------------------------- |
| `unshared`     | `SCHED_UNSHARED_HELD` / `NONE`                      | 当前唯一 lock 的 key          | 覆盖 + token 恢复        | 按 lock key 统计 waiter，决定是否扩展时间片 |
| `atomic_dense` | `SCHED_ATOMIC_DENSE` / `NONE`                       | 原子变量集合的 exact set hash | XOR 累加 + clear/restore | 原子密集任务按集合 key 共置                 |
| `dep_role`     | `SCHED_DEP_NONE` / `PRODUCER` / `CONSUMER` / `BOTH` | 当前交接对象的 key            | 覆盖 + token 恢复        | 生产者消费者模型的任务按交接对象 key 共置   |

这三个标签的 magic 都是 **exact key**：只有 key 完全相等，才表示同一个 lock / 集合 / 交接对象。

- pointer key：`hash64(ptr ^ per-process salt)`
- static key：`hash64(id)`，用于跨进程对象
- key 是 64-bit 双射；调度器只看到 hash，不看到裸 VA
- 嵌套通过 `struct sched_tag_token` 保存/恢复外层状态

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
    struct sched_tag_token t;
    int lock = 0;
    int counter_a = 0, counter_b = 0;
    int chan = 0;

    /* unshared：当前 lock，嵌套安全 */
    t = sched_tag_enter_unshared(&lock);
    critical_section();
    sched_tag_leave_unshared(&t);

    /* atomic：当前原子变量集合 key */
    t = sched_tag_enter_atomic();
    sched_tag_add_atomic(&counter_a);
    sched_tag_add_atomic(&counter_b);
    lock_free_region();
    sched_tag_leave_atomic(&t);

    /* dep：当前 pipe + role */
    t = sched_tag_enter_dep(&pipe, SCHED_DEP_CONSUMER);
    recv(pipe);
    sched_tag_leave_dep(&t);

    /* 跨进程 / 静态 id */
    t = sched_tag_enter_unshared_id(0xDEADBEEF12345678ULL);
    critical_section();
    sched_tag_leave_unshared(&t);
}
```

编译并链接：

```bash
cc -Iinclude -I/path/to/kernel/usr/include your.c libsched_tag.a -o your
```

`unshared` / `atomic` / `dep` 都有 `_id` 变体，用于静态 ID 或跨进程对象。

## 兼容接口 `sched_tag_set()`

`sched_tag_set()` 保留给旧代码和简单场景。它**不保存/恢复外层状态**，所以嵌套区域仍应使用 `enter_*` / `leave_*`。

### 两种参数形式

```c
/* 只写 state byte，不带 magic */
sched_tag_set(TYPE, value);

/* 写 state + exact key（C11 用 _Generic 自动区分指针和整数） */
sched_tag_set(TYPE, value, &object);   /* pointer key */
sched_tag_set(TYPE, value, 0xDEADBEEF); /* static id key */
```

- 指针形式：`key = hash64(ptr ^ per-process salt)`
- 整数形式：`key = hash64(id)`，用于跨进程对象
- `value = 0` 时，对应 magic 会被清 0

### 每种标签的语义

```c
/* 非作用域标签：只有 state */
sched_tag_set(FUNIT_DENSE, SCHED_FUNIT_INT);
sched_tag_set(MEMORY_DENSE, SCHED_MEMORY_STREAM);
sched_tag_set(LOAD_AHEAD, SCHED_LOAD_RISING);
sched_tag_set(FUNIT_DENSE, SCHED_FUNIT_NONE); /* 清空 */

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
struct sched_tag_token t = sched_tag_enter_atomic();
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

`test/reader_tokens.c` 用 fake slot 验证 token 嵌套、exact key、atomic set hash、
dep role 和兼容 setter，不需要支持 `PR_SET_SCHED_HINT` 的内核。

## 分支

- `master`：源码 API + 库 + token API 测试
- `deprecated/pass`：旧的 LLVM Pass / SchedQL / JSON 工具链快照
