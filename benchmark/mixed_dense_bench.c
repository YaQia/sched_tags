/*
 * mixed_dense_bench.c — MIXED compute-dense benchmark
 *
 * This benchmark interleaves integer and floating-point operations so that
 * neither INT nor FLOAT alone reaches the density threshold, but their
 * combined compute ratio does.
 *
 * BB threshold = 0.70, Loop threshold = 0.50.
 *
 * Example: 6 INT ops + 6 FLOAT ops + 2 non-compute (terminator, etc.)
 *          INT ratio  = 6/14 ≈ 0.43 < 0.70  → not INT
 *          FLOAT ratio = 6/14 ≈ 0.43 < 0.70  → not FLOAT
 *          Combined   = 12/14 ≈ 0.86 >= 0.70 → MIXED
 */

#include <stdint.h>
#include <stdio.h>

__attribute__((noinline))
double mixed_dense_block(int32_t x, double y)
{
    /* INT ops */
    int32_t a = x + 0x1111;        /* add    */
    a = a ^ 0x2222;                /* xor    */
    a = a * 7;                     /* mul    */
    a = a - 0x3333;                /* sub    */
    a = a | 0x00FF;                /* or     */
    a = a & 0xF0F0;               /* and    */
    a = a ^ (a >> 3);              /* ashr + xor */
    a = a + (a << 2);              /* shl  + add */

    /* FLOAT ops */
    double b = y + 1.5;            /* fadd   */
    b = b * 2.7;                   /* fmul   */
    b = b - 0.3;                   /* fsub   */
    b = b * 3.14;                  /* fmul   */
    b = b + (double)a;             /* sitofp + fadd */
    b = b - 0.77;                  /* fsub   */
    b = b * 1.01;                  /* fmul   */
    b = b + 0.5;                   /* fadd   */

    return b;
    /* ~10 INT ops + ~8 FLOAT ops + 1 ret
     * Neither alone >= 0.70, but combined ≈ 18/19 ≈ 0.95 → MIXED */
}

__attribute__((noinline))
double mixed_dense_loop(int32_t n, double seed)
{
    int32_t acc_i = 0;
    double  acc_f = seed;

    for (int32_t i = 0; i < n; i++) {
        /* INT ops */
        acc_i = acc_i + i;          /* add    */
        acc_i = acc_i ^ (i * 3);    /* mul + xor */
        acc_i = acc_i - (i >> 1);   /* ashr + sub */
        acc_i = acc_i | 0xFF;       /* or     */

        /* FLOAT ops */
        acc_f = acc_f + (double)i;  /* sitofp + fadd */
        acc_f = acc_f * 1.0001;     /* fmul   */
        acc_f = acc_f - 0.5;        /* fsub   */
        acc_f = acc_f + 0.25;       /* fadd   */
    }

    return acc_f + (double)acc_i;
}

int main(void)
{
    volatile double sink;

    /* BB-level MIXED */
    sink = mixed_dense_block(42, 3.14);

    /* Loop-level MIXED */
    sink = mixed_dense_loop(10000000, (double)sink);

    printf("result = %f\n", (double)sink);
    return 0;
}
