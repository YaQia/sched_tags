#===----------------------------------------------------------------------===#
# SchedTag pass plugin shared library
#===----------------------------------------------------------------------===#

# Build a loadable module (.so / .dylib) for use with opt's -load-pass-plugin
add_library(SchedTagPass MODULE
  SchedTagPass.cpp

  # Additional source files go here:
  # AnotherFile.cpp
)

target_include_directories(SchedTagPass PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${LLVM_INCLUDE_DIRS}
)

target_compile_definitions(SchedTagPass PRIVATE
  ${LLVM_DEFINITIONS}
)
