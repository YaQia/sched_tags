/*
 * int_dense_bench.c — INT compute-dense benchmark
 *
 * Design goals for triggering SCHED_TAG_COMPUTE_DENSE / SCHED_COMPUTE_INT:
 *
 *   BB-level :  >= 10 instructions per basic block, INT ratio >= 70%
 *   Loop-level: >= 10 instructions total in loop,   INT ratio >= 50%
 *
 * Key rules from the pass (SchedTagPass.cpp  computeOpType):
 *   - Load / Store / PHI / Select / Alloca / Terminator / Call  → NONE
 *   - Add, Sub, Mul, UDiv, SDiv, Shl, LShr, AShr, And, Or, Xor,
 *     ICmp, Trunc, ZExt, SExt, PtrToInt, IntToPtr              → INT
 *
 * Strategy:
 *   1. Use a tight loop with MANY pure integer ALU ops per iteration.
 *   2. Avoid function calls inside the hot loop (calls count as NONE).
 *   3. Use `volatile` sink to prevent the optimizer from removing work.
 *   4. Keep loads/stores to a minimum (only the volatile sink).
 */

#include <stdint.h>
#include <stdio.h>

/* Prevent link-time / IPO inlining from collapsing everything. */
__attribute__((noinline))
int32_t int_dense_loop(int32_t n)
{
    int32_t a = 0x12345678;
    int32_t b = 0x0ABCDEF0;
    int32_t c = 0x55AA33CC;
    int32_t d = 0x0F0F0F0F;

    /*
     * Each iteration contains ~30 pure integer ALU operations
     * (add, sub, mul, xor, and, or, shl, ashr, icmp …)
     * plus only 1 icmp + 1 br (terminator) + 1 phi for the counter.
     * INT ratio ≈ 30 / 34 ≈ 0.88 — well above the 0.50 loop threshold
     * and the 0.70 BB threshold.
     */
    for (int32_t i = 0; i < n; i++) {
        a = a + b;              /* add  */
        b = b ^ c;              /* xor  */
        c = c - d;              /* sub  */
        d = d | a;              /* or   */

        a = a * 31;             /* mul  */
        b = b & 0xFF00FF00;     /* and  */
        c = c ^ (a >> 3);       /* ashr + xor  */
        d = d + (b << 5);       /* shl  + add  */

        a = a - (c & 0x0F0F);   /* and  + sub  */
        b = b ^ (d * 7);        /* mul  + xor  */
        c = c + (a | 0xAAAA);   /* or   + add  */
        d = d - (b & c);        /* and  + sub  */

        a = (a << 2) | (a >> 30);  /* shl + ashr + or  (rotate) */
        b = (b ^ 0xDEADBEEF) + c;  /* xor + add  */
        c = (c - d) * 3;           /* sub + mul  */
        d = (d + a) ^ (b - c);     /* add + xor + sub */
    }

    return a ^ b ^ c ^ d;          /* xor + xor + xor */
}

/*
 * Standalone basic-block level: a single straight-line block with
 * >= 10 INT ops and zero calls.   INT ratio > 70%.
 */
__attribute__((noinline))
int32_t int_dense_block(int32_t x)
{
    int32_t r = x;
    r = r + 0x11111111;         /* add  */
    r = r ^ 0x22222222;         /* xor  */
    r = r - 0x33333333;         /* sub  */
    r = r * 37;                 /* mul  */
    r = r | 0x00FF00FF;         /* or   */
    r = r & 0xF0F0F0F0;         /* and  */
    r = r ^ (r >> 7);           /* ashr + xor  */
    r = r + (r << 3);           /* shl  + add  */
    r = r - 0xAAAAAAAA;         /* sub  */
    r = r * 17;                 /* mul  */
    r = r ^ (r >> 11);          /* ashr + xor  */
    r = r + (r << 4);           /* shl  + add  */
    return r;
    /* Total ≈ 16 INT ops, 0 calls, 1 ret  → ratio ≈ 16/17 ≈ 0.94 */
}

int main(void)
{
    volatile int32_t sink;

    /* Loop-level INT-dense region */
    sink = int_dense_loop(10000000);

    /* BB-level INT-dense region */
    sink = int_dense_block((int32_t)sink);

    printf("result = %d\n", (int)sink);
    return 0;
}
