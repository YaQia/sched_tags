; test/test_atomic.ll — Test instrumentation of atomic-dense regions.
;
; Functions:
;   @atomic_bb_work     — Atomic-dense BB (many atomicrmw ops)
;   @atomic_loop_work   — Atomic-dense loop (CAS-heavy retry loop)
;   @mixed_atomic_bb    — Non-dense loop + standalone atomic-dense BB
;   @trivial            — No atomic ops → should NOT be instrumented
;
; observe_hint is defined in reader_atomic.c; called inside dense regions
; so the reader can verify tags_active WHILE the region is executing.

declare void @observe_hint(i32)

; ============================================================
; Atomic-dense BB: 8 atomicrmw + 1 call + 1 add + 1 ret = 11 total
; Ratio = 8/11 ≈ 0.73 > 0.6 BB threshold → dense
; ============================================================
define i32 @atomic_bb_work(ptr %p) {
entry:
  %v0 = atomicrmw add ptr %p, i32 1 seq_cst
  %v1 = atomicrmw sub ptr %p, i32 1 seq_cst
  %v2 = atomicrmw xor ptr %p, i32 255 seq_cst
  %v3 = atomicrmw or  ptr %p, i32 15 seq_cst
  call void @observe_hint(i32 1)
  %v4 = atomicrmw and ptr %p, i32 -16 seq_cst
  %v5 = atomicrmw add ptr %p, i32 %v0 seq_cst
  %v6 = atomicrmw xchg ptr %p, i32 0 seq_cst
  %v7 = atomicrmw add ptr %p, i32 42 seq_cst
  %sum = add i32 %v0, %v7
  ret i32 %sum
}

; ============================================================
; Atomic-dense loop: 6 atomicrmw + 2 phi + 1 call + 1 add + 1 add
;                    + 1 icmp + 1 br = 13 total
; Ratio = 6/13 ≈ 0.46 > 0.4 loop threshold → dense
; ============================================================
define i32 @atomic_loop_work(i32 %n, ptr %p) {
entry:
  br label %loop.preheader

loop.preheader:
  br label %loop.body

loop.body:
  %i   = phi i32 [ 0, %loop.preheader ], [ %i.next, %loop.body ]
  %acc = phi i32 [ 0, %loop.preheader ], [ %sum, %loop.body ]
  %v0 = atomicrmw add ptr %p, i32 1 seq_cst
  %v1 = atomicrmw sub ptr %p, i32 1 seq_cst
  %v2 = atomicrmw xor ptr %p, i32 %i seq_cst
  call void @observe_hint(i32 10)
  %v3 = atomicrmw add ptr %p, i32 %v0 seq_cst
  %v4 = atomicrmw or  ptr %p, i32 15 seq_cst
  %v5 = atomicrmw and ptr %p, i32 -1 seq_cst
  %sum = add i32 %acc, %v0
  %i.next = add i32 %i, 1
  %done = icmp eq i32 %i.next, %n
  br i1 %done, label %loop.exit, label %loop.body

loop.exit:
  ret i32 %sum
}

; ============================================================
; mixed_atomic_bb: non-dense counting loop + standalone atomic-dense BB.
; The loop body has no atomic ops → not atomic-dense.
; The standalone BB has 6 atomicrmw + 1 call + 1 br = 8 total
; → 6/8 = 0.75 > 0.6 → BB-level SET+CLR.
; ============================================================
define i32 @mixed_atomic_bb(i32 %n, ptr %p) {
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
  br i1 %cond, label %atomic_bb, label %done

atomic_bb:
  ; 6 atomicrmw + 1 call + 1 br = 8 total → 6/8 = 0.75 > 0.6
  %a0 = atomicrmw add ptr %p, i32 1 seq_cst
  %a1 = atomicrmw sub ptr %p, i32 1 seq_cst
  %a2 = atomicrmw xor ptr %p, i32 255 seq_cst
  call void @observe_hint(i32 30)
  %a3 = atomicrmw or  ptr %p, i32 15 seq_cst
  %a4 = atomicrmw and ptr %p, i32 -16 seq_cst
  %a5 = atomicrmw xchg ptr %p, i32 0 seq_cst
  br label %done

done:
  %result = phi i32 [ %j.next, %count.exit ], [ %a5, %atomic_bb ]
  ret i32 %result
}

; ============================================================
; trivial — no atomic ops at all → no instrumentation
; ============================================================
define i32 @trivial(i32 %a) {
entry:
  %r = add i32 %a, 1
  ret i32 %r
}

; ============================================================
; cas_retry_loop: A lock-free CAS retry loop.
; Short loop with exactly 1 cmpxchg, back-edge controlled by success.
; Ratio is low (1/7 < 0.4) but should be detected as atomic-dense.
; ============================================================
define void @cas_retry_loop(ptr %p) {
entry:
  br label %loop

loop:
  %old = load atomic i32, ptr %p monotonic, align 4
  %new = add i32 %old, 1
  call void @observe_hint(i32 40)
  %res = cmpxchg ptr %p, i32 %old, i32 %new seq_cst seq_cst
  %res_old = extractvalue { i32, i1 } %res, 0
  %success = icmp eq i32 %res_old, %old
  br i1 %success, label %exit, label %loop

exit:
  ret void
}
