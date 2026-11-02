# sched_tag

源码级的调度标签 API + 一个轻量库 `libsched_tag.a`。

LLVM Pass、SchedQL、JSON 配置等实验性工具链已从 master 移除，完整代码保存在
`deprecated/pass` 分支。master 只保留稳定的、可审查的源码 API。

## 语义

三个标签的 magic 都是 **exact 64-bit identity key**：

| 标签 | magic | 写入方式 | 调度器 |
|---|---|---|---|
| `unshared` | 当前唯一 lock 的 key | 覆盖 + token 恢复 | 按 lock key 统计 waiter |
| `atomic_dense` | 原子变量集合的 exact set hash | XOR 累加 + clear/restore | 按集合 identity 共置 |
| `dep` | 当前 pipe 的 key | 覆盖 + token 恢复 | pipe + role 配对 |

- pointer key：`hash64(ptr ^ per-process salt)`
- static key：`hash64(id)`，用于跨进程对象
- key 是 64-bit 双射；调度器只看到 hash，不看到裸 VA
- 嵌套通过 `struct sched_tag_token` 保存/恢复外层状态

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
    int pipe = 0;

    /* unshared：当前 lock，嵌套安全 */
    t = sched_tag_enter_unshared(&lock);
    critical_section();
    sched_tag_leave_unshared(&t);

    /* atomic：当前原子变量集合 identity */
    t = sched_tag_enter_atomic();
    sched_tag_add_atomic(&counter_a);
    sched_tag_add_atomic(&counter_b);
    lock_free_region();
    sched_tag_leave_atomic(&t);

    /* dep：当前 pipe + role */
    t = sched_tag_enter_dep(&pipe, SCHED_DEP_CONSUMER);
    recv(pipe);
    sched_tag_leave_dep(&t);

    /* 跨进程 / 静态 identity */
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
`funit_dense` / `memory_dense` / `load_ahead` 仍可通过兼容接口
`sched_tag_set()` 写入。

## 读取

```c
uint8_t state = sched_tag_get(UNSHARED);

/* exact key 比较；不是 bloom/popcount */
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
