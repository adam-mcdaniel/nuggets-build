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

#include "PhaseBoundPass.hh"

// Instrument the marker basic blocks with the corresponding marker functions
bool PhaseBoundPass::instrumentMarkerBBs(Module &M,
        const uint64_t warmup_marker_bb_id,
        const uint64_t start_marker_bb_id,
        const uint64_t end_marker_bb_id,
        bool no_warmup_marker) {
    
    // [nuggetkit] Hooks are declared on demand (see getHook).
    FunctionCallee warmup_marker_hook_function =
        getHook(M, "nugget_warmup_marker_hook", 0);
    FunctionCallee start_marker_hook_function =
        getHook(M, "nugget_start_marker_hook", 0);
    FunctionCallee end_marker_hook_function =
        getHook(M, "nugget_end_marker_hook", 0);

    std::vector<std::pair<uint64_t, FunctionCallee>> markers_to_instrument = {
        {start_marker_bb_id, start_marker_hook_function},
        {end_marker_bb_id, end_marker_hook_function}
    };
    
    if (!no_warmup_marker) {
        markers_to_instrument.push_back(
            {warmup_marker_bb_id, warmup_marker_hook_function});
    }

    // Find the basic blocks with the given bb_ids and instrument them
    LLVMContext &Context = M.getContext();
    int64_t bb_id = -1;
    for (Function &F : M) {
        if (F.isDeclaration()) continue;

        if (std::find(nugget_functions.begin(), nugget_functions.end(),
                      F.getName().str()) != nugget_functions.end()) {
            continue;
        }
        
        for (BasicBlock &BB : F) {
            // Get the bb_id metadata
            bb_id = -1;
            Instruction *T = BB.getTerminator();
            if (T) {
                MDNode* bb_id_md = T->getMetadata(kBbIdKey);
                if (!bb_id_md) {
                    continue;
                }
                
                // Cast operand to MDString and extract the value
                MDString *bb_id_str = dyn_cast<MDString>(
                                                    bb_id_md->getOperand(0));
                if (!bb_id_str) {
                    continue;
                }
                bb_id = std::stoll(bb_id_str->getString().str());
            } else {
                continue;
            }
            assert(bb_id != -1 && "bb_id should have been set from metadata");

            // [nuggetkit] Several markers may share one BB (e.g. a loop
            // latch that ends both the previous and the measured region).
            // Upstream erased from the vector while iterating over it, which
            // is undefined behaviour and dropped all but one marker.
            bool hit = false;
            for (const auto &marker : markers_to_instrument) {
                if ((uint64_t)bb_id == marker.first) {
                    callBefore(T, marker.second, {});
                    hit = true;
                }
            }
            if (hit) {
                markers_to_instrument.erase(
                    std::remove_if(
                        markers_to_instrument.begin(),
                        markers_to_instrument.end(),
                        [&](const std::pair<uint64_t, FunctionCallee> &m) {
                            return m.first == (uint64_t)bb_id;
                        }),
                    markers_to_instrument.end());
            }

            if (markers_to_instrument.empty()) {
                return true; // All markers have been instrumented
            }
        }
        if (markers_to_instrument.empty()) {
            return true; // All markers have been instrumented
        }
    }

    // If we reach here, some markers were not found
    return false;
}

// Label the marker basic blocks with inline assembly markers
bool PhaseBoundPass::labelMarkerBBs(Module &M,
    const uint64_t warmup_marker_bb_id,
    const uint64_t start_marker_bb_id,
    const uint64_t end_marker_bb_id,
    bool no_warmup_marker) {

    std::vector<std::pair<uint64_t, std::string>> markers_to_instrument = {
        {start_marker_bb_id, "nugget_start_marker:\n\tnop\n"},
        {end_marker_bb_id, "nugget_end_marker:\n\tnop\n"}
    };
    
    if (!no_warmup_marker) {
        markers_to_instrument.push_back(
            {warmup_marker_bb_id, "nugget_warmup_marker:\n\tnop\n"});
    }

    // Find the basic blocks with the given bb_ids and instrument them
    LLVMContext &Context = M.getContext();
    IRBuilder<> builder(Context);
    int64_t bb_id = -1;
    for (Function &F : M) {
        if (F.isDeclaration()) continue;

        if (std::find(nugget_functions.begin(), nugget_functions.end(),
                      F.getName().str()) != nugget_functions.end()) {
            continue;
        }
        
        for (BasicBlock &BB : F) {
            // Get the bb_id metadata
            bb_id = -1;
            Instruction *T = BB.getTerminator();
            if (T) {
                MDNode* bb_id_md = T->getMetadata(kBbIdKey);
                if (!bb_id_md) {
                    continue;
                }
                
                // Cast operand to MDString and extract the value
                MDString *bb_id_str = dyn_cast<MDString>(
                                                    bb_id_md->getOperand(0));
                if (!bb_id_str) {
                    continue;
                }
                bb_id = std::stoll(bb_id_str->getString().str());
            } else {
                continue;
            }
            assert(bb_id != -1 && "bb_id should have been set from metadata");

            bool hit = false;
            for (const auto &marker : markers_to_instrument) {
                if ((uint64_t)bb_id == marker.first) {
                    builder.SetInsertPoint(T);
                    auto *asmTy = FunctionType::get(builder.getVoidTy(), false);
                    std::string constraints = "~{memory}";
                    auto *ia = InlineAsm::get(asmTy, marker.second, constraints, /*hasSideEffects=*/true);
                    builder.CreateCall(ia);
                    hit = true;
                }
            }
            if (hit) {
                markers_to_instrument.erase(
                    std::remove_if(
                        markers_to_instrument.begin(),
                        markers_to_instrument.end(),
                        [&](const std::pair<uint64_t, std::string> &m) {
                            return m.first == (uint64_t)bb_id;
                        }),
                    markers_to_instrument.end());
            }

            if (markers_to_instrument.empty()) {
                return true; // All markers have been instrumented
            }
        }
        if (markers_to_instrument.empty()) {
            return true; // All markers have been instrumented
        }
    }

    // If we reach here, some markers were not found
    return false;

}

PreservedAnalyses PhaseBoundPass::run(Module &M,
                                      ModuleAnalysisManager &MAM) {
    LLVMContext &Context = M.getContext();
    
    uint64_t warmup_marker_bb_id = std::stoull(
        GetOptionValue(options_, "warmup_marker_bb_id"));
    uint64_t warmup_marker_count = std::stoull(
        GetOptionValue(options_, "warmup_marker_count"));
    uint64_t start_marker_bb_id = std::stoull(
        GetOptionValue(options_, "start_marker_bb_id"));
    uint64_t start_marker_count = std::stoull(
        GetOptionValue(options_, "start_marker_count"));
    uint64_t end_marker_bb_id = std::stoull(
        GetOptionValue(options_, "end_marker_bb_id"));
    uint64_t end_marker_count = std::stoull(
        GetOptionValue(options_, "end_marker_count"));
    bool label_only =
        GetOptionValue(options_, "label_only") == "true" ? true : false;
    DEBUG_PRINT("PhaseBoundPass options:"
        << "\n  warmup_marker_bb_id: " << warmup_marker_bb_id
        << "\n  warmup_marker_count: " << warmup_marker_count
        << "\n  start_marker_bb_id: " << start_marker_bb_id
        << "\n  start_marker_count: " << start_marker_count
        << "\n  end_marker_bb_id: " << end_marker_bb_id
        << "\n  end_marker_count: " << end_marker_count
        << "\n  label_only: " << (label_only ? "true" : "false")
    );

    // Instrument the `nugget_init` function to `nugget_roi_begin_` with the
    // marker counts
    std::vector<Value*> args;
    args.push_back(
            ConstantInt::get(Type::getInt64Ty(Context), warmup_marker_count));
    args.push_back(
            ConstantInt::get(Type::getInt64Ty(Context), start_marker_count));
    args.push_back(
            ConstantInt::get(Type::getInt64Ty(Context), end_marker_count));
    if (!instrumentRoiBegin(M, args)) {
        report_fatal_error("Error instrumenting nugget_roi_begin_");
    }

    // Instrument the marker basic blocks with the corresponding marker 
    // functions
    if (label_only) {
        if (!labelMarkerBBs(
            M, warmup_marker_bb_id, start_marker_bb_id, end_marker_bb_id, warmup_marker_count == 0)) {
            report_fatal_error("Error labeling marker basic blocks");
        }
        return PreservedAnalyses::all();
    }
    // Otherwise, instrument the marker BBs
    if (!instrumentMarkerBBs(
            M, warmup_marker_bb_id, start_marker_bb_id, end_marker_bb_id, warmup_marker_count == 0)) {
        report_fatal_error("Error instrumenting marker basic blocks");
    }
    return PreservedAnalyses::all();
}
