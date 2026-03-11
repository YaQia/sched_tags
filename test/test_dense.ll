; test/test_dense.ll — Test instrumentation of compute-dense BBs.
;
; This function has multiple BBs with different compute characteristics:
;   entry     → mixed (not dense)
;   int_work  → INT-dense  (contains observe_hint callback for runtime check)
;   float_work → FLOAT-dense (contains observe_hint callback for runtime check)
;   merge     → not dense (successor of dense BBs → should get CLEAR)
;
; observe_hint is defined in reader.c; called inside dense regions so the
; reader can verify tags_active WHILE the region is executing.

declare void @observe_hint(i32)

define i32 @workload(i32 %n, double %x) {
entry:
  %cond = icmp sgt i32 %n, 10
  br i1 %cond, label %int_work, label %float_work

int_work:
  ; 10 int ops + 1 call + 1 br = 12 total → 10/12 ≈ 0.83 > 0.7
  %a0 = add i32 %n, 1
  %a1 = mul i32 %a0, %n
  %a2 = sub i32 %a1, %a0
  %a3 = sdiv i32 %a2, %n
  %a4 = shl i32 %a3, %a0
  call void @observe_hint(i32 1)
  %a5 = xor i32 %a4, %a3
  %a6 = and i32 %a5, %a2
  %a7 = or  i32 %a6, %a1
  %a8 = add i32 %a7, %a0
  %a9 = mul i32 %a8, %n
  br label %merge

float_work:
  ; 10 float ops + 1 call + 1 fptosi + 1 br = 13 total → 10/13 ≈ 0.77 > 0.7
  %f0 = fadd double %x, 1.0
  %f1 = fmul double %f0, %x
  %f2 = fsub double %f1, %f0
  %f3 = fdiv double %f2, %x
  %f4 = fadd double %f3, %f0
  call void @observe_hint(i32 2)
  %f5 = fmul double %f4, %x
  %f6 = fsub double %f5, %f0
  %f7 = fdiv double %f6, %x
  %f8 = fadd double %f7, %f0
  %f9 = fmul double %f8, %x
  %fi = fptosi double %f9 to i32
  br label %merge

merge:
  %result = phi i32 [ %a9, %int_work ], [ %fi, %float_work ]
  ret i32 %result
}

; A simple SIMD-only function.
define <4 x float> @simd_kernel(<4 x float> %a, <4 x float> %b) {
entry:
  %0 = fadd <4 x float> %a, %b
  %1 = fmul <4 x float> %0, %a
  %2 = fsub <4 x float> %1, %b
  %3 = fdiv <4 x float> %2, %a
  %4 = fadd <4 x float> %3, %b
  %5 = fmul <4 x float> %4, %a
  %6 = fsub <4 x float> %5, %b
  %7 = fdiv <4 x float> %6, %a
  ret <4 x float> %7
}

; A function with no dense BBs — should NOT be instrumented.
define i32 @trivial(i32 %a) {
entry:
  %0 = add i32 %a, 1
  ret i32 %0
}
