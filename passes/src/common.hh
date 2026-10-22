// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Zhantong Qiu
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Common utilities and helper functions for Nugget LLVM passes.
//
// This header provides shared functionality used across multiple pass
// implementations, including:
//   - Debug printing macros
//   - LLVM header includes with version compatibility
//   - Option parsing for parameterized passes
//   - Pass name matching utilities
//
// The option parsing system supports LLVM new pass manager syntax:
//   -passes="pass-name<option1=value1;option2=value2>"

#ifndef _COMMON_HH_
#define _COMMON_HH_

// DEBUG_PRINT macro - prints debug messages when DEBUG is defined.
//
// Usage: DEBUG_PRINT("Processing function: " << F.getName());
//
// This macro expands to nothing in release builds (when DEBUG is not defined),
// avoiding runtime overhead. In debug builds (-DDEBUG), it prints to stderr
// via LLVM's errs() stream with a "DEBUG: " prefix.
#ifdef DEBUG
#define DEBUG_PRINT(msg) errs() << "DEBUG: " << msg << "\n"
#else
#define DEBUG_PRINT(msg)
#endif

// LLVM Core Headers
// PassBuilder provides the new pass manager infrastructure for registering
// and configuring optimization passes.
#include "llvm/Passes/PassBuilder.h"

// PassPlugin.h location varies across LLVM versions.
// LLVM 16+: llvm/Plugins/PassPlugin.h
// LLVM 15 and earlier: llvm/Passes/PassPlugin.h
// Use __has_include to select correct header at compile time.
#if __has_include("llvm/Plugins/PassPlugin.h")
  #include "llvm/Plugins/PassPlugin.h"
#else
  #include "llvm/Passes/PassPlugin.h"
#endif

// LLVM IR Representation Headers
#include "llvm/IR/Module.h"        // LLVM module (translation unit)
#include "llvm/IR/Function.h"      // Function representation
#include "llvm/IR/Metadata.h"      // Metadata nodes for annotations
#include "llvm/IR/IRBuilder.h"     // IR construction helper
#include "llvm/IR/InlineAsm.h"      // Inline assembly support
#include "llvm/IR/BasicBlock.h"    // Basic block representation
#include "llvm/IR/PassManager.h"   // Pass manager infrastructure
#include "llvm/IR/Instructions.h"  // Instruction classes
#include "llvm/IR/DebugInfoMetadata.h" // DILocation for inserted calls

// LLVM Support Utilities
#include "llvm/Support/Error.h"         // Error handling (Expected<T>)
#include "llvm/Support/FileSystem.h"    // File I/O operations
#include "llvm/Support/raw_ostream.h"   // Stream output (errs(), outs())

// Standard Library Headers
#include <string>   // std::string
#include <vector>   // std::vector

using namespace llvm;

// Metadata key for basic block ID annotations.
//
// This constant defines the metadata node name used to attach unique
// identifiers to basic blocks in LLVM IR. The IRBBLabelPass creates
// metadata nodes with this key on terminator instructions.
//
// Example IR:
//   br label %if.then, !bb.id !42
//   !42 = !{!"5"}
//
// The numeric string ("5") represents the unique basic block ID.
static constexpr const char *kBbIdKey = "bb.id";

// Option structure for parameterized pass configuration.
//
// Stores key-value pairs parsed from pass parameter strings in LLVM's
// new pass manager. Pass parameters use the syntax:
//   -passes="pass-name<key1=value1;key2=value2>"
//
// Example:
//   -passes="ir-bb-label-pass<output_csv=my_file.csv>"
//   Results in: Options{"output_csv", "my_file.csv"}
//
// Fields:
//   option_name: Parameter key (e.g., "output_csv")
//   option_value: Parameter value (e.g., "my_file.csv")
struct Options {
  std::string option_name;   // Parameter name/key
  std::string option_value;  // Parameter value
  
  // Checks if option has been assigned a value.
  //
  // Returns:
  //   true if option_value is non-empty, false otherwise
  bool is_set() const {
    return !option_value.empty();
  }
};

// Helper function names related to initialization and roi bounding.
// Use the fully mangled (trailing underscore) names so we reliably skip
// instrumenting the helper/runtime functions themselves. Instrumenting them
// causes self-recursion (e.g., nugget_bb_hook_ calling itself) and test
// failures/segfaults.
const std::vector<std::string> nugget_functions = {
  "nugget_init",
  "nugget_roi_begin_",
  "nugget_roi_end_",
  "nugget_bb_hook",
  "nugget_warmup_marker_hook",
  "nugget_start_marker_hook",
  "nugget_end_marker_hook"
};

// Helper function to get the option value by name.
static std::string GetOptionValue(const std::vector<Options> &options, 
                                                    const std::string &name) {
  for (const auto &opt : options) {
    if (opt.option_name == name) {
      return opt.option_value;
    }
  }
  return "";
}

// Parses pass parameter string into Options vector.
//
// Takes a semicolon-separated parameter string and extracts key-value pairs,
// validating them against expected options. Used by parameterized passes to
// configure behavior via command-line arguments.
//
// Parameter format:
//   "key1=value1;key2=value2;key3=value3"
//
// Args:
//   Params: Parameter string from pass invocation (e.g., 
//                                                       "output_csv=file.csv")
//   TargetOptions: Vector of expected options with default values
//
// Returns:
//   Expected<std::vector<Options>>: Parsed options on success, or error on:
//     - Malformed key=value pairs (missing = or empty key/value)
//     - Unknown option keys not in TargetOptions
//     - Missing required options (options with empty default values)
//
// Example:
//   Input: "output_csv=results.csv;threshold=100"
//   Target: {{"output_csv", "bb_info.csv"}, {"threshold", ""}}
//   Output: {{"output_csv", "results.csv"}, {"threshold", "100"}}
//
// Implementation note:
//   Uses simple O(n*m) search since typical option count is < 5.
static Expected<std::vector<Options>> ParseOptions(StringRef Params, 
                            const std::vector<Options> TargetOptions) {
  // Split parameter string by semicolons into individual key=value pairs.
  
  SmallVector<StringRef, 8> Items;
  Params.split(Items, ';', /*MaxSplit=*/-1, /*KeepEmpty=*/false);

  std::vector<Options> ReturnOptions = TargetOptions;

  for (StringRef It : Items) {
    It = It.trim();
    if (It.empty())
      continue;

    auto KV = It.split('=');
    StringRef K = KV.first.trim();
    StringRef V = KV.second.trim();

    if (K.empty() || V.empty()) {
      return make_error<StringError>("invalid option: " + It, 
                                                    inconvertibleErrorCode());
    }
    
    bool Found = false;
    for (auto &Opt : ReturnOptions) {
      DEBUG_PRINT("comparing K='" << K.str() << "' with Opt.option_name='" 
                                                    << Opt.option_name << "'");
      if (Opt.option_name == K.str()) {
        Opt.option_value = V.str();
        DEBUG_PRINT("Match found! Set " << Opt.option_name << " = " 
                                                          << Opt.option_value);
        Found = true;
        break;
      }
    }
    if (!Found) {
      return make_error<StringError>("unknown option: " + K.str(), 
                                                    inconvertibleErrorCode());
    }
  }

  // Check all required options are set
  for (const auto &Opt : ReturnOptions) {
    DEBUG_PRINT("Checking if set: " << Opt.option_name << " = '" 
      << Opt.option_value << "' is_set=" << (Opt.is_set() ? "true" : "false"));
    if (!Opt.is_set()) {
      return make_error<StringError>("missing required option: " + 
                                    Opt.option_name, inconvertibleErrorCode());
    }
  }

  DEBUG_PRINT("parseOptions returning successfully");
  return ReturnOptions;
}

// Matches pass name and extracts parameters if present.
//
// Determines if a pass invocation string matches the expected pass name
// and parses any parameters if the pass is invoked in parameterized form.
// Supports both bare pass names and parameterized syntax.
//
// Supported formats:
//   1. Bare pass name: "ir-bb-label-pass"
//      - Uses default values from TargetOptions
//   2. Parameterized: "ir-bb-label-pass<output_csv=custom.csv>"
//      - Parses parameters and overrides defaults
//
// Args:
//   Name: Full pass invocation string from -passes=...
//   Base: Expected base pass name (e.g., "ir-bb-label-pass")
//   TargetOptions: Expected options with default values
//
// Returns:
//   Expected<std::vector<Options>>: Parsed/default options on success, or 
//   error:
//     - "name not matched": Name doesn't start with Base (not this pass)
//     - "malformed parameterized pass name": Invalid <...> syntax
//     - ParseOptions errors: Parameter parsing failures
//     - "missing required option": Required option has no default and not 
//       provided
//
// Examples:
//   MatchParamPass("ir-bb-label-pass", "ir-bb-label-pass", opts)
//     -> Uses default values from opts
//
//   MatchParamPass("ir-bb-label-pass<output_csv=out.csv>", "ir-bb-label-pass",
//   opts)
//     -> Parses "output_csv=out.csv" and returns modified opts
//
//   MatchParamPass("other-pass", "ir-bb-label-pass", opts)
//     -> Returns "name not matched" error (different pass)
static Expected<std::vector<Options>> MatchParamPass(StringRef Name, 
                  StringRef Base, const std::vector<Options> TargetOptions) {
  DEBUG_PRINT("MatchParamPass: Name='" << Name << "' Base='" << Base << "'");
  if (Name == Base) {
    // No parameters
    for (const auto &Opt : TargetOptions) {
      if (!Opt.is_set()) {
        return make_error<StringError>("missing required option: " + 
                                    Opt.option_name, inconvertibleErrorCode());
      }
    }
    return std::vector<Options>(TargetOptions);
  }
  if (!Name.starts_with(Base)) {
    return make_error<StringError>("name not matched", 
                                                    inconvertibleErrorCode());
  }
  if (Name.size() <= Base.size() + 2) { // must fit "<>"
    return make_error<StringError>("malformed parameterized pass name", 
                                                    inconvertibleErrorCode());
  }
  if (Name[Base.size()] != '<' || !Name.ends_with(">")) {
    return make_error<StringError>("malformed parameterized pass name", 
                                                    inconvertibleErrorCode());
  }
  StringRef Params = Name.drop_front(Base.size() + 1).drop_back(1);
  DEBUG_PRINT("extracted Params='" << Params << "'");
  return ParseOptions(Params, TargetOptions);
}

// [nuggetkit] Declare (or reuse) the runtime hook `void name(i64, ...)` with
// `nargs` arguments. Upstream required each hook to be already declared in
// the module, which forced the hook source to be linked as bitcode and broke
// when two passes need different nugget_init signatures.
static FunctionCallee getHook(Module &M, StringRef name, unsigned nargs) {
  LLVMContext &C = M.getContext();
  SmallVector<Type *, 4> params(nargs, Type::getInt64Ty(C));
  return M.getOrInsertFunction(
      name, FunctionType::get(Type::getVoidTy(C), params, false));
}

// [nuggetkit] Insert `F(args)` before `T`. The call carries a debug location
// whenever the function has debug info: the IR verifier rejects calls
// without one in functions with a DISubprogram ("inlinable function call in
// a function with debug info must have a !dbg location").
static void callBefore(Instruction *T, FunctionCallee F,
                       ArrayRef<Value *> args) {
  IRBuilder<> builder(T);
  CallInst *call = builder.CreateCall(F, args);
  if (T->getDebugLoc()) {
    call->setDebugLoc(T->getDebugLoc());
  } else if (DISubprogram *SP = T->getFunction()->getSubprogram()) {
    call->setDebugLoc(DILocation::get(T->getContext(), SP->getLine(), 0, SP));
  }
}

static bool instrumentRoiBegin(Module &M,
                              std::vector<Value*> args) {
  // Insert nugget_init call at the end of nugget_roi_begin_ (before return)
  Function* roi_begin_function = M.getFunction("nugget_roi_begin_");
  if (!roi_begin_function || roi_begin_function->isDeclaration()) {
    errs() << "Function nugget_roi_begin_ not defined in the module\n";
    return false;
  }
  callBefore(roi_begin_function->back().getTerminator(),
             getHook(M, "nugget_init", args.size()), args);
  return true;
}

#endif // _COMMON_HH_
