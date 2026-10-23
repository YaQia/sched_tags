cmake_minimum_required(VERSION 3.20)
project(SchedTag LANGUAGES C CXX)

#===----------------------------------------------------------------------===#
# Find LLVM package
#===----------------------------------------------------------------------===#
find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

#===----------------------------------------------------------------------===#
# LLVM build configuration
#===----------------------------------------------------------------------===#
set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# LLVM is normally built without RTTI. Match that setting.
if(NOT LLVM_ENABLE_RTTI)
  if(MSVC)
    string(APPEND CMAKE_CXX_FLAGS " /GR-")
  else()
    string(APPEND CMAKE_CXX_FLAGS " -fno-rtti")
  endif()
endif()

#===----------------------------------------------------------------------===#
# Runtime library: libsched_tag.a
#===----------------------------------------------------------------------===#
# Single instance of the per-thread sched_hint support: the TLS slot cache, the
# prctl() registrar, and the fork hook. Instrumented code (manual header use or
# the LLVM pass) only references __sched_hint_ptr / __sched_hint_register; this
# library defines them, so there is exactly one .init_array entry.
set(KERNEL_HEADERS "" CACHE PATH
    "Optional extra include dir holding linux/sched_hint.h (uninstalled kernel tree)")

add_library(sched_tag STATIC
  sched_tag.c
)
target_include_directories(sched_tag PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${KERNEL_HEADERS}
)

#===----------------------------------------------------------------------===#
# Pass plugin target
#===----------------------------------------------------------------------===#
add_subdirectory(pass)
