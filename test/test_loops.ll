; test/test_loops.ll — Test loop-level and BB-level instrumentation.
;
; Functions:
;   @int_loop_dense   — INT-dense loop (>50% int ops) → loop-level SET/CLR
;   @float_loop_dense — FLOAT-dense loop (>50% float ops) → loop-level SET/CLR
;   @simd_loop_dense  — SIMD-dense loop → loop-level SET/CLR
;   @mixed_with_dense_bb — non-dense loop + standalone INT-dense BB → BB fallback
;   @trivial          — no instrumentation
;
; observe_hint is defined in reader_loops.c; called inside dense regions so
; the reader can verify tags_active WHILE the region is executing.

declare void @observe_hint(i32)

; ============================================================
; INT-dense loop: 12 int compute ops + 1 call + 1 icmp + 1 br = 15 total
; Ratio = 12/15 = 0.80 > 0.5 threshold → dense
; ============================================================
define i32 @int_loop_dense(i32 %n) {
entry:
  br label %loop.preheader

loop.preheader:
  br label %loop.body

loop.body:
  %i   = phi i32 [ 0, %loop.preheader ], [ %i.next, %loop.body ]
  %acc = phi i32 [ 0, %loop.preheader ], [ %r10, %loop.body ]
  %r0  = add i32 %acc, %i
  %r1  = mul i32 %r0, %n
  %r2  = sub i32 %r1, %i
  %r3  = shl i32 %r2, 2
  %r4  = xor i32 %r3, %r1
  %r5  = and i32 %r4, %r2
  call void @observe_hint(i32 10)
  %r6  = or  i32 %r5, %r0
  %r7  = add i32 %r6, %r3
  %r8  = mul i32 %r7, %r4
  %r9  = sub i32 %r8, %r5
  %r10 = add i32 %r9, 1
  %i.next = add i32 %i, 1
  %done = icmp eq i32 %i.next, %n
  br i1 %done, label %loop.exit, label %loop.body

loop.exit:
  ret i32 %r10
}

; ============================================================
; FLOAT-dense loop: 12 float ops + 1 call + 1 add + 1 icmp + 1 br = 16
; Ratio = 12/16 = 0.75 > 0.5 → dense
; ============================================================
define double @float_loop_dense(i32 %n, double %seed) {
entry:
  br label %loop.preheader

loop.preheader:
  br label %loop.body

loop.body:
  %i   = phi i32 [ 0, %loop.preheader ], [ %i.next, %loop.body ]
  %acc = phi double [ %seed, %loop.preheader ], [ %f10, %loop.body ]
  %f0  = fadd double %acc, 1.0
  %f1  = fmul double %f0, 2.0
  %f2  = fsub double %f1, %acc
  %f3  = fdiv double %f2, 3.0
  %f4  = fadd double %f3, %f0
  %f5  = fmul double %f4, %f1
  call void @observe_hint(i32 20)
  %f6  = fsub double %f5, %f2
  %f7  = fdiv double %f6, %f3
  %f8  = fadd double %f7, %f4
  %f9  = fmul double %f8, %f5
  %f10 = fadd double %f9, %f6
  %f11 = fmul double %f10, %f7
  %i.next = add i32 %i, 1
  %done = icmp eq i32 %i.next, %n
  br i1 %done, label %loop.exit, label %loop.body

loop.exit:
  ret double %f11
}

; ============================================================
; SIMD-dense loop: vector fadd/fmul/fsub/fdiv on <4 x float>
; 12 vector ops + 1 add + 1 icmp + 1 br = 15 total → 0.80 > 0.5
; (no observe_hint here — simd_loop_dense takes vector args, hard to call
;  from C; kept for pass analysis verification only)
; ============================================================
define <4 x float> @simd_loop_dense(i32 %n, <4 x float> %a, <4 x float> %b) {
entry:
  br label %loop.preheader

loop.preheader:
  br label %loop.body

loop.body:
  %i   = phi i32 [ 0, %loop.preheader ], [ %i.next, %loop.body ]
  %v   = phi <4 x float> [ %a, %loop.preheader ], [ %v11, %loop.body ]
  %v0  = fadd <4 x float> %v, %b
  %v1  = fmul <4 x float> %v0, %a
  %v2  = fsub <4 x float> %v1, %b
  %v3  = fdiv <4 x float> %v2, %a
  %v4  = fadd <4 x float> %v3, %b
  %v5  = fmul <4 x float> %v4, %a
  %v6  = fsub <4 x float> %v5, %b
  %v7  = fdiv <4 x float> %v6, %a
  %v8  = fadd <4 x float> %v7, %b
  %v9  = fmul <4 x float> %v8, %a
  %v10 = fsub <4 x float> %v9, %b
  %v11 = fadd <4 x float> %v10, %a
  %i.next = add i32 %i, 1
  %done = icmp eq i32 %i.next, %n
  br i1 %done, label %loop.exit, label %loop.body

loop.exit:
  ret <4 x float> %v11
}

; ============================================================
; mixed_with_dense_bb: contains a non-dense loop (just counting)
; plus a standalone INT-dense BB outside the loop.
; The loop body has mostly phi/icmp/br → not dense.
; The standalone BB has 11 int ops + 1 call + 1 br = 13 total
; → 11/13 ≈ 0.85 > 0.7 → BB-level SET+CLR.
; ============================================================
define i32 @mixed_with_dense_bb(i32 %n) {
entry:
  br label %count.preheader

count.preheader:
  br label %count.body

count.body:
  %j = phi i32 [ 0, %count.preheader ], [ %j.next, %count.body ]
  %j.next = add i32 %j, 1
  %cnt.done = icmp eq i32 %j.next, %n
  br i1 %cnt.done, label %count.exit, label %count.body

count.exit:
  %cond = icmp sgt i32 %n, 100
  br i1 %cond, label %dense_bb, label %done

dense_bb:
  ; 11 int compute ops + 1 call + 1 br = 13 total → 11/13 ≈ 0.85 > 0.7
  %d0 = add i32 %n, 42
  %d1 = mul i32 %d0, 7
  %d2 = sub i32 %d1, %d0
  %d3 = shl i32 %d2, 3
  %d4 = xor i32 %d3, %d1
  %d5 = and i32 %d4, %d2
  call void @observe_hint(i32 30)
  %d6 = or  i32 %d5, %d0
  %d7 = add i32 %d6, %d3
  %d8 = mul i32 %d7, %d4
  %d9 = sub i32 %d8, %d5
  %d10 = add i32 %d9, 1
  br label %done

done:
  %result = phi i32 [ %j.next, %count.exit ], [ %d10, %dense_bb ]
  ret i32 %result
}

; ============================================================
; trivial — no dense regions at all → no instrumentation
; ============================================================
define i32 @trivial(i32 %a) {
entry:
  %r = add i32 %a, 1
  ret i32 %r
}
