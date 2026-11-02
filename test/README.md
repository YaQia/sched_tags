# test/

`reader_tokens.c` 是纯用户态 token API 单元测试：

- 用 fake slot 提供 `__sched_hint_ptr` / `__sched_tag_process_salt` /
  `__sched_hint_register`
- 不需要支持 `PR_SET_SCHED_HINT` 的内核
- 覆盖：
  - unshared 嵌套 save/restore
  - atomic exact set hash、部分重叠差异、clear/restore
  - dep PRODUCER / CONSUMER / BOTH / 静态 id
  - 兼容 `sched_tag_set()`

运行：

```bash
make check
# 或者
cc -std=c11 -Iinclude -I/path/to/kernel/usr/include \
   test/reader_tokens.c -o test/reader_tokens
./test/reader_tokens
```

旧的 LLVM Pass 读者测试、SchedQL 测试和 JSON 配置测试保存在
`deprecated/pass` 分支。
