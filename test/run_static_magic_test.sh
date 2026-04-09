#!/bin/bash
set -e
echo "1. Writing C test file..."
echo "2. Writing JSON tags file with value: 3735928559..."
clang -O1 -S -emit-llvm test/static_magic_test.c -o test/static_magic_test.ll
opt -load-pass-plugin=build/pass/libSchedTagPass.so -passes=sched-tag -sched-tags-file=test/static_magic_tags.json -S test/static_magic_test.ll -o test/static_magic_test_opt.ll > /dev/null 2>&1
echo "3. Extracting instructions from output IR..."
cat test/static_magic_test_opt.ll | awk '/define dso_local void @worker()/,/ret void/'
