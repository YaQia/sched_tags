#===----------------------------------------------------------------------===#
# SchedTag pass plugin shared library
#===----------------------------------------------------------------------===#

# Build a loadable module (.so / .dylib) for use with opt's -load-pass-plugin
add_library(SchedTagPass MODULE
  SchedTagCommon.cpp
  ComputeDenseAnalysis.cpp
  AtomicDenseAnalysis.cpp
  SchedQLParser.cpp
  SourceLabelAnalysis.cpp
  SchedTagPass.cpp
)

target_include_directories(SchedTagPass PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

target_compile_definitions(SchedTagPass PRIVATE
  ${LLVM_DEFINITIONS}
)

# For an out-of-tree pass plugin loaded via opt's -load-pass-plugin, all LLVM
# symbols are resolved at load time from the host process (opt). We must NOT
# link against static LLVM libs (they may not exist). Instead, we use
# LLVM_LINK_LLVM_DYLIB to link against the single shared libLLVM.so when
# available, or simply rely on runtime symbol resolution.
if(LLVM_LINK_LLVM_DYLIB)
  # LLVM was built with a shared libLLVM.so — link against it.
  target_link_libraries(SchedTagPass PRIVATE LLVM)
else()
  # No shared libLLVM — symbols come from the opt host binary at runtime.
  # On Linux, use -Wl,--unresolved-symbols=ignore-in-shared-libs so the
  # linker doesn't complain about unresolved LLVM symbols in our .so.
  if(NOT WIN32)
    target_link_options(SchedTagPass PRIVATE
      "LINKER:--unresolved-symbols=ignore-in-shared-libs"
    )
  endif()
endif()
