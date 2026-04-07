; test/test_source_labels.ll — Test source labels from sched_tags.json
;
; This file tests:
;   1. Single query labels (compute-dense, atomic-dense, etc.)
;   2. Ranged query labels (unshared with start/end)
;   3. Loop-level source labels
;   4. BB-level source labels
;   5. Direct instruction query labels
;
; Each function is designed to be matched by corresponding entries in
; sched_tags_test.json.

declare void @observe_hint(i32)

; External functions to simulate lock/unlock (will be linked with reader)
declare void @mutex_lock(ptr)
declare void @mutex_unlock(ptr)

;; ==========================================================================
;; Test 1: compute-dense via source label (not auto-analysis)
;; The function name is matched by sched_tags.json, not by density analysis.
;; ==========================================================================

define i32 @labeled_compute(i32 %n) {
entry:
  ; This BB will be labeled as compute-dense via source label
  ; even though it doesn't meet auto-analysis density threshold
  call void @observe_hint(i32 1)
  %r = add i32 %n, 42
  ret i32 %r
}

;; ==========================================================================
;; Test 2: Loop with source label (atomic-dense)
;; ==========================================================================

define i32 @labeled_atomic_loop(i32 %n, ptr %counter) {
entry:
  br label %loop

loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  ; atomicrmw to trigger loop[contains=atomicrmw] pattern
  %old = atomicrmw add ptr %counter, i32 1 seq_cst
  call void @observe_hint(i32 10)
  %i.next = add i32 %i, 1
  %cond = icmp slt i32 %i.next, %n
  br i1 %cond, label %loop, label %exit

exit:
  ret i32 %i.next
}

;; ==========================================================================
;; Test 3: Ranged label (unshared) - mutex lock/unlock pattern
;; The lock and unlock calls define the precise range.
;; ==========================================================================

@global_mutex = global i64 0

define i32 @critical_section(i32 %x) {
entry:
  ; mutex_lock call - start of unshared region
  call void @mutex_lock(ptr @global_mutex)
  
  ; Inside critical section - should have unshared=1
  call void @observe_hint(i32 20)
  %r = mul i32 %x, 2
  
  ; mutex_unlock call - end of unshared region
  call void @mutex_unlock(ptr @global_mutex)
  
  ; After unlock - should have unshared=0
  call void @observe_hint(i32 21)
  
  ret i32 %r
}

;; ==========================================================================
;; Test 4: Another ranged label test with different function
;; ==========================================================================

define i32 @another_critical(i32 %a, i32 %b) {
entry:
  %sum = add i32 %a, %b
  
  call void @mutex_lock(ptr @global_mutex)
  call void @observe_hint(i32 30)  ; inside critical section
  %prod = mul i32 %sum, 2
  call void @mutex_unlock(ptr @global_mutex)
  
  call void @observe_hint(i32 31)  ; outside critical section
  ret i32 %prod
}

;; ==========================================================================
;; Test 5: Function that should NOT be labeled (control group)
;; ==========================================================================

define i32 @unlabeled_function(i32 %n) {
entry:
  call void @observe_hint(i32 99)
  %r = add i32 %n, 1
  ret i32 %r
}

;; ==========================================================================
;; Test 6: io-dense label via source label
;; ==========================================================================

define void @io_operation(ptr %buf, i32 %size) {
entry:
  ; This will be marked as io-dense via source label
  call void @observe_hint(i32 40)
  ret void
}

;; ==========================================================================
;; Test 7: branch-dense label via source label  
;; ==========================================================================

define i32 @branchy_code(i32 %x, i32 %y) {
entry:
  call void @observe_hint(i32 50)
  %c1 = icmp sgt i32 %x, 0
  br i1 %c1, label %then1, label %else1

then1:
  %r1 = add i32 %x, 1
  br label %merge

else1:
  %r2 = sub i32 %x, 1
  br label %merge

merge:
  %r = phi i32 [ %r1, %then1 ], [ %r2, %else1 ]
  ret i32 %r
}

;; ==========================================================================
;; Test 8: BB entry block query - bb[entry]
;; ==========================================================================

define i32 @bb_test_entry(i32 %n) {
entry:
  ; This entry block should be matched by bb[entry]
  call void @observe_hint(i32 60)
  %r = add i32 %n, 1
  ret i32 %r
}

;; ==========================================================================
;; Test 9: BB contains atomicrmw query - bb[contains=atomicrmw]
;; ==========================================================================

define i32 @bb_test_contains(ptr %counter) {
entry:
  call void @observe_hint(i32 70)
  br label %atomic_bb

atomic_bb:
  ; This BB should be matched by bb[contains=atomicrmw]
  %old = atomicrmw add ptr %counter, i32 1 seq_cst
  call void @observe_hint(i32 71)
  ret i32 %old
}

;; ==========================================================================
;; Test 10: BB combined patterns - bb[entry;contains=call]
;; ==========================================================================

define i32 @bb_test_combined(i32 %n) {
entry:
  ; This entry block contains call, should match bb[entry;contains=call]
  call void @observe_hint(i32 80)
  %r = add i32 %n, 1
  ret i32 %r
}
