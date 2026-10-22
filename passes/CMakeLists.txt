# NuggetPasses.so: the Nugget LLVM passes (ir-bb-label-pass,
# phase-analysis-pass, phase-bound-pass), vendored from
# https://github.com/studyztp/Nugget-LLVM-passes (commit in UPSTREAM) with the
# fixes marked [nuggetkit] in src/. Build against the LLVM whose clang emits
# the app's bitcode and whose opt runs the passes:
#
#   cmake -S passes -B build/passes -DLLVM_DIR=$LLVM/lib/cmake/llvm && cmake --build build/passes
#
# `nugget setup` does this for you.
cmake_minimum_required(VERSION 3.20)
project(NuggetPasses LANGUAGES CXX)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "NuggetPasses: LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)

add_llvm_pass_plugin(NuggetPasses
  src/PluginRegistration.cpp
  src/IRBBLabelPass.cpp
  src/PhaseAnalysisPass.cpp
  src/PhaseBoundPass.cpp
)
target_include_directories(NuggetPasses PRIVATE ${LLVM_INCLUDE_DIRS})
target_compile_definitions(NuggetPasses PRIVATE ${LLVM_DEFINITIONS})
target_compile_features(NuggetPasses PRIVATE cxx_std_17)
