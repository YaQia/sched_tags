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
# Pass plugin target
#===----------------------------------------------------------------------===#
add_subdirectory(pass)
