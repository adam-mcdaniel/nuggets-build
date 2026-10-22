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


#include "IRBBLabelPass.hh"

// IRBBLabelPass::run - Main pass execution function.
//
// Processes the entire LLVM module to assign unique IDs to all basic blocks,
// attach metadata, and export collected information to CSV.
//
// Algorithm:
//   1. Initialize counters for functions and basic blocks
//   2. Iterate through all functions in the module
//      - Skip function declarations (F.isDeclaration())
//      - For each basic block:
//        a. Assign globally unique ID
//        b. Attach !bb.id metadata to terminator instruction
//        c. Collect basic block statistics
//   3. Write collected data to CSV file
//
// Args:
//   M: LLVM Module to instrument
//   AM: Module analysis manager (unused)
//
// Returns:
//   PreservedAnalyses::all() - Metadata doesn't invalidate analyses
PreservedAnalyses IRBBLabelPass::run(Module &M, ModuleAnalysisManager &) {
    LLVMContext &C = M.getContext();
    
    // Initialize counters for assigning unique IDs
    uint64_t function_counter = 0;            // Function ID counter
    uint64_t basic_block_global_counter = 0;  // Global BB ID counter
    bb_info_list_.clear();  // Clear any existing data from previous runs

    // Iterate through all functions in the module
    for (Function &F : M) {
        // Skip function declarations (external functions without definitions)
        // isDeclaration() returns true for functions like printf, malloc, etc.
        if (F.isDeclaration()) continue;

        // Skip function if it is one of the nugget helper functions
        if (std::find(nugget_functions.begin(), nugget_functions.end(),
                      F.getName().str()) != nugget_functions.end()) {
            continue;
        }

        // Process each basic block in the function
        for (BasicBlock &BB : F) {
            // Assign globally unique basic block ID and increment counter
            uint64_t bb_id = basic_block_global_counter++;

            // Attach !bb.id metadata to the terminator instruction
            // Terminator is the last instruction in a BB (the branch 
            // instruction)
            Instruction *T = BB.getTerminator();
            if (T) {
                // Create metadata node: !bb.id !N where !N = !{"<bb_id>"}
                // The ID is stored as a string within an MDString node
                MDNode *N = MDNode::get(C, MDString::get(
                                                    C, std::to_string(bb_id)));
                T->setMetadata(kBbIdKey, N);
            } else {
                // Fatal error if basic block has no terminator (malformed IR)
                // This should never happen with valid LLVM IR
                report_fatal_error(Twine("BasicBlock ") + BB.getName() +
                        " in function " + F.getName() +
                        " has no terminator instruction.");
            }

            // Collect basic block information for CSV export
            BasicBlockInfo bb_info;
            // Function name (mangled for C++)
            bb_info.function_name = F.getName().str();  
            // Current function's ID
            bb_info.function_id = function_counter;   
            // BB label ("" for entry block)   
            bb_info.basic_block_name = BB.getName().str();  
            // Number of instructions
            bb_info.basic_block_inst_count = BB.size(); 
            // Globally unique BB ID 
            bb_info.basic_block_id = bb_id;              

            bb_info_list_.push_back(bb_info);
        }
        // Increment function counter after processing all BBs in this function
        function_counter++;
    }

    // Export collected basic block information to CSV file
    // File path is specified in options_[0].option_value (default: 
    //                                                          "bb_info.csv")
    std::error_code EC;
    raw_fd_ostream csv_file(GetOptionValue(options_, "output_csv"), EC, sys::fs::OF_Text);
    if (EC) {
        report_fatal_error(Twine("Error opening file ") + GetOptionValue(options_, "output_csv")
               + ": " + EC.message());
    }
    
    // Write CSV header row
    csv_file << "FunctionName,FunctionID,BasicBlockName,"
                                    << "BasicBlockInstCount,BasicBlockID\n";
    
    // Write data rows (one per basic block)
    for (const auto &bb_info : bb_info_list_) {
        csv_file << bb_info.function_name << ","
                << bb_info.function_id << ","
                // Empty string is allowed
                << bb_info.basic_block_name << ","  
                << bb_info.basic_block_inst_count << ","
                << bb_info.basic_block_id << "\n";
    }
    csv_file.close();
    
    // Return PreservedAnalyses::all() because metadata addition doesn't
    // invalidate any existing analyses (CFG, dominators, etc. remain valid)
    return PreservedAnalyses::all();
}
