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
#ifndef _IRBBLABELPASS_HH_
#define _IRBBLABELPASS_HH_

#include "common.hh"
// IRBBLabelPass - Basic block instrumentation and labeling pass.
//
// This LLVM pass instruments IR basic blocks with unique identifiers via
// metadata and exports basic block information to a CSV file. The pass:
//   1. Assigns globally unique IDs to each basic block in the module
//   2. Attaches !bb.id metadata to terminator instructions  
//   3. Collects basic block statistics (name, instruction count, function)
//   4. Exports data to CSV for analysis and validation
//
// Usage:
//   opt -load-pass-plugin=NuggetPasses.so \
//       -passes="ir-bb-label-pass<output_csv=results.csv>" \
//       input.ll -o output.bc
//
// CSV Output Format:
//   FunctionName,FunctionID,BasicBlockName,BasicBlockInstCount,BasicBlockID
//   main,0,,5,0
//   main,0,if.then,3,1
//   main,0,if.end,2,2
//
// The pass is parameterized with the following options:
//   output_csv: Output CSV filename (default: bb_info.csv)

// Contains default values for pass parameters. Users can override these
// via parameterized pass invocation syntax.
static const std::vector<Options> IRBBLabelPassOptions = {
    {"output_csv", "bb_info.csv"}  // Default output file name
};

class IRBBLabelPass : public PassInfoMixin<IRBBLabelPass> {
  public:
    // Constructor - Initializes pass with configuration options.
    //
    // Args:
    //   Options: Vector of option key-value pairs
    IRBBLabelPass(std::vector<Options> Options) {
        options_ = Options;
    }
    ~IRBBLabelPass() = default;

    // BasicBlockInfo - Stores metadata about a single basic block.
    //
    // Contains all relevant information collected during pass execution for
    // a single basic block, including its relationship to its parent function.
    //
    // Fields are exported to CSV in the order declared.
    struct BasicBlockInfo {
        // Function metadata
        std::string function_name;          // Function name (mangled for C++)
        uint64_t function_id;               // Unique function identifier

        // Basic block metadata
        std::string basic_block_name;       // BB label (empty for entry block)
        uint64_t basic_block_inst_count;    // Number of instructions in BB
        uint64_t basic_block_id;            // Globally unique BB identifier
    };

  private:
    std::vector<BasicBlockInfo> bb_info_list_;  // Collected BB information
    std::vector<Options> options_;              // Pass configuration options

  public:
    // Main pass entry point - processes entire module.
    //
    // Iterates through all defined functions and their basic blocks,
    // assigning unique IDs and collecting statistics. After processing,
    // exports data to CSV file specified in options.
    //
    // Args:
    //   M: LLVM Module to process
    //   AM: Module analysis manager (unused but required by pass interface)
    //
    // Returns:
    //   PreservedAnalyses::all() - Pass does not invalidate any analyses
    //     (metadata addition is transparent to analysis passes)
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &);
};

#endif // _IRBBLABELPASS_HH_