; ModuleID = 'test/static_magic_test.ll'
source_filename = "test/static_magic_test.c"
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

%struct.sched_hint = type { i32, i32, i8, i8, i8, i8, i8, i8, i8, i8, i64, i64, i64, i8, [23 x i8] }

@__sched_hint.data = weak thread_local(initialexec) global %struct.sched_hint { i32 1397246286, i32 1, i8 0, i8 0, i8 0, i8 0, i8 0, i8 0, i8 0, i8 0, i64 0, i64 0, i64 0, i8 0, [23 x i8] zeroinitializer }, section "__sched_hint", align 64
@llvm.used = appending global [1 x ptr] [ptr @__sched_hint.data], section "llvm.metadata"
@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [{ i32, ptr, ptr } { i32 65535, ptr @__sched_hint_report, ptr null }]

; Function Attrs: nounwind sspstrong uwtable
define dso_local void @worker() local_unnamed_addr #0 {
  store i8 1, ptr getelementptr inbounds nuw (%struct.sched_hint, ptr @__sched_hint.data, i32 0, i32 7), align 1
  store i64 3735928559, ptr getelementptr inbounds nuw (%struct.sched_hint, ptr @__sched_hint.data, i32 0, i32 12), align 8
  tail call void @my_cross_process_lock() #3
  tail call void @my_cross_process_unlock() #3
  store i8 0, ptr getelementptr inbounds nuw (%struct.sched_hint, ptr @__sched_hint.data, i32 0, i32 7), align 1
  store i64 0, ptr getelementptr inbounds nuw (%struct.sched_hint, ptr @__sched_hint.data, i32 0, i32 12), align 8
  ret void
}

declare void @my_cross_process_lock() local_unnamed_addr #1

declare void @my_cross_process_unlock() local_unnamed_addr #1

define internal void @__sched_hint_report() {
entry:
  %tp = call ptr @llvm.thread.pointer.p0()
  %tp.int = ptrtoint ptr %tp to i64
  %offset = sub i64 ptrtoint (ptr @__sched_hint.data to i64), %tp.int
  %0 = call i32 (i32, ...) @prctl(i32 83, i64 %offset, i64 ptrtoint (ptr @__sched_hint.data to i64), i32 0, i32 0)
  ret void
}

; Function Attrs: nocallback nofree nosync nounwind willreturn memory(none)
declare ptr @llvm.thread.pointer.p0() #2

declare i32 @prctl(i32, ...)

attributes #0 = { nounwind sspstrong uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #1 = { "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #2 = { nocallback nofree nosync nounwind willreturn memory(none) }
attributes #3 = { nounwind }

!llvm.module.flags = !{!0, !1, !2, !3}
!llvm.ident = !{!4}
!llvm.errno.tbaa = !{!5}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 8, !"PIC Level", i32 2}
!2 = !{i32 7, !"PIE Level", i32 2}
!3 = !{i32 7, !"uwtable", i32 2}
!4 = !{!"clang version 22.1.2"}
!5 = !{!6, !6, i64 0}
!6 = !{!"int", !7, i64 0}
!7 = !{!"omnipotent char", !8, i64 0}
!8 = !{!"Simple C/C++ TBAA"}
