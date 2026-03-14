// test/test_rust_dense.rs — Rust compute-dense test cases for SchedTag pass.
//
// Covers INT-dense, FLOAT-dense, and MIXED-dense regions.
// Each function calls observe_hint(tag_id) from INSIDE the dense region
// so the reader can verify that tags_active is set while executing.
//
// Build workflow:
//   rustc --emit=llvm-ir -C opt-level=1 -C overflow-checks=no \
//         -C no-prepopulate-passes --crate-type=lib \
//         test/test_rust_dense.rs -o /tmp/test_rust_dense.ll
//   opt -O1 -load-pass-plugin=build/pass/libSchedTagPass.so \
//       -passes=sched-tag /tmp/test_rust_dense.ll -o /tmp/tagged_rust.bc
//   llc /tmp/tagged_rust.bc -filetype=obj -o /tmp/tagged_rust.o
//   clang /tmp/tagged_rust.o test/reader_rust.c -lm -o /tmp/test_rust
//   /tmp/test_rust

extern "C" {
    fn observe_hint(tag_id: i32);
}

/// INT-dense: many integer ALU ops, one observe_hint call, one return.
/// ~14 int ops + 1 call + 1 ret = 16 → 14/16 ≈ 0.875 > 0.70
#[no_mangle]
#[inline(never)]
pub extern "C" fn rust_int_work(n: i32) -> i32 {
    let a0 = n.wrapping_add(1);
    let a1 = a0.wrapping_mul(n);
    let a2 = a1.wrapping_sub(a0);
    let a3 = a2 ^ n;
    let a4 = a3 & 0x7FFF_FFFF;
    let a5 = a4 | a0;
    let a6 = a5.wrapping_add(a2);

    unsafe {
        observe_hint(1);
    }

    let a7 = a6.wrapping_mul(a3);
    let a8 = a7 ^ a4;
    let a9 = a8.wrapping_sub(a5);
    let a10 = a9.wrapping_add(a6);
    let a11 = a10 & 0xFF00_FF00u32 as i32;
    let a12 = a11 | a7;
    let a13 = a12.wrapping_add(1);
    a13
}

/// FLOAT-dense: many f64 ops, one observe_hint call, one fptosi + return.
/// ~14 float ops + 1 call + 1 fptosi + 1 ret = 17 → 14/17 ≈ 0.82 > 0.70
#[no_mangle]
#[inline(never)]
pub extern "C" fn rust_float_work(x: f64) -> i32 {
    let f0 = x + 1.0;
    let f1 = f0 * x;
    let f2 = f1 - f0;
    let f3 = f2 / x;
    let f4 = f3 + f0;
    let f5 = f4 * f1;
    let f6 = f5 - f2;

    unsafe {
        observe_hint(2);
    }

    let f7 = f6 / f3;
    let f8 = f7 + f4;
    let f9 = f8 * f5;
    let f10 = f9 - f6;
    let f11 = f10 + f7;
    let f12 = f11 * f8;
    let f13 = f12 - f9;
    f13 as i32
}

/// MIXED-dense: interleaved int + float ops so that neither alone
/// reaches the 70% BB threshold, but combined they do.
/// ~8 int ops + ~8 float ops + 1 sitofp + 1 call + 1 ret = 19
/// INT  ratio = 8/19 ≈ 0.42 < 0.70
/// FLOAT ratio = 8/19 ≈ 0.42 < 0.70 (sitofp counts as INT)
/// Combined   = 17/19 ≈ 0.89 >= 0.70 → MIXED
#[no_mangle]
#[inline(never)]
pub extern "C" fn rust_mixed_work(n: i32, x: f64) -> f64 {
    // Integer block
    let a0 = n.wrapping_add(0x1111);
    let a1 = a0 ^ 0x2222;
    let a2 = a1.wrapping_mul(7);
    let a3 = a2.wrapping_sub(a0);

    // Float block
    let f0 = x + 1.5;
    let f1 = f0 * 2.7;
    let f2 = f1 - 0.3;
    let f3 = f2 * 3.14;

    unsafe {
        observe_hint(3);
    }

    // More integer
    let a4 = a3 | 0x00FF;
    let a5 = a4 & 0xF0F0;
    let a6 = a5 ^ a2;
    let a7 = a6.wrapping_add(a3);

    // More float
    let f4 = f3 + f0;
    let f5 = f4 * 1.01;
    let f6 = f5 - 0.77;
    let f7 = f6 + 0.5;

    f7 + (a7 as f64)
}

/// INT-dense loop: pure integer loop body.
/// ~10 int ops + 1 call + 1 icmp + 1 br + 2 phi ≈ 15 total
/// INT ratio = 10/15 ≈ 0.67 > 0.50 loop threshold
#[no_mangle]
#[inline(never)]
pub extern "C" fn rust_int_loop(n: i32) -> i32 {
    let mut acc: i32 = 0;
    let mut i: i32 = 0;
    while i < n {
        acc = acc.wrapping_add(i);
        acc = acc.wrapping_mul(3);
        acc = acc ^ (i.wrapping_add(1));
        acc = acc.wrapping_sub(i >> 1);
        acc = acc | 0xFF;
        acc = acc & 0x7FFF_FFFF;
        acc = acc.wrapping_add(i ^ 0x55);
        acc = acc ^ (acc >> 4);
        acc = acc.wrapping_add(1);

        if i == n / 2 {
            unsafe {
                observe_hint(10);
            }
        }

        i = i.wrapping_add(1);
    }
    acc
}

/// Trivial function — no dense regions, should NOT be instrumented.
#[no_mangle]
#[inline(never)]
pub extern "C" fn rust_trivial(a: i32) -> i32 {
    a.wrapping_add(1)
}
