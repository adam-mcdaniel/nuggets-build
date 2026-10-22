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
#include "PhaseAnalysisPass.hh"
#include "PhaseBoundPass.hh"

//
// Plugin registration and pass manager integration.
//
// This file implements the LLVM pass plugin interface, registering custom 
// passes with the new pass manager. 
// The llvmGetPassPluginInfo() function is the plugin
// entry point called by LLVM's plugin system when loading the shared library.
//
// Pass Registration Flow:
//   1. opt loads NuggetPasses.so via -load-pass-plugin=...
//   2. LLVM calls llvmGetPassPluginInfo() to get plugin information
//   3. Plugin registers pipeline parsing callback
//   4. When -passes="..." is parsed, callback checks for registered pass names
//   5. If match found, instantiates pass and adds to pipeline
//
// Currently registered passes:
//   - ir-bb-label-pass: Basic block labeling and CSV export
//   - phase-analysis-pass: Phase detection instrumentation
//   - phase-bound-pass: ROI marker instrumentation

// Plugin entry point - provides plugin metadata and registration callbacks.
//
// This extern "C" function is called by LLVM's plugin loader when the shared
// library is loaded. It must have C linkage and the exact name
// "llvmGetPassPluginInfo" for LLVM to find it.
//
// LLVM_ATTRIBUTE_WEAK allows multiple passes to be compiled into separate
// plugins without symbol conflicts.
//
// Returns:
//   PassPluginLibraryInfo structure containing:
//     - API version (LLVM_PLUGIN_API_VERSION)
//     - Plugin name ("NuggetPasses")
//     - LLVM version string
//     - Callback lambda for registering passes
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
    return {LLVM_PLUGIN_API_VERSION, "NuggetPasses", LLVM_VERSION_STRING,
        // Registration callback - invoked during pass pipeline construction
        [](PassBuilder &PB) {
            // Register pipeline parsing callback for -passes="..." argument
            // This callback is invoked for each pass name in the pipeline 
            // string
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                    DEBUG_PRINT("Pipeline parsing callback called with Name='" 
                                                            << Name << "'");
                    
                    // Attempt to match "ir-bb-label-pass" with optional 
                    // parameters
                    // MatchParamPass handles both forms:
                    //   1. "ir-bb-label-pass" (uses default options)
                    //   2. "ir-bb-label-pass<output_csv=custom.csv>" 
                    //       (parsed options)
                    auto E = MatchParamPass(Name, "ir-bb-label-pass", 
                                                        IRBBLabelPassOptions);
                    if (E) {
                        // Success: add pass to module pass manager with parsed
                        // options
                        MPM.addPass(IRBBLabelPass(*E));
                        return true;
                    } else {
                        // Error: check if it was a real error or just name 
                        // mismatch
                        std::string ErrorMsg = toString(E.takeError());
                        // Only report errors if pass name matched but 
                        // parameter parsing failed
                        // (Don't spam errors for every unrelated pass name in 
                        // pipeline)
                        if (ErrorMsg.find("name not matched") 
                                                == std::string::npos) {
                            errs() << "ir-bb-label-pass param parse error: " 
                                                        << ErrorMsg << "\n";
                            return false;
                        }
                    }
                    
                    // Check if it's the PhaseAnalysisPass
                    auto E2 = MatchParamPass(Name, "phase-analysis-pass", 
                                              PhaseAnalysisPassOptions);
                    if (E2) {
                        MPM.addPass(PhaseAnalysisPass(*E2));
                        return true;
                    } else {
                        std::string ErrorMsg = toString(E2.takeError());
                        if (ErrorMsg.find("name not matched") 
                                                == std::string::npos) {
                            errs() << "phase-analysis-pass param parse error: " 
                                                        << ErrorMsg << "\n";
                            return false;
                        }
                    }

                    // Check if it's the PhaseBoundPass
                    auto E3 = MatchParamPass(Name, "phase-bound-pass",
                                                PhaseBoundPassOptions);
                    if (E3) {
                        MPM.addPass(PhaseBoundPass(*E3));
                        return true;
                    } else {
                        std::string ErrorMsg = toString(E3.takeError());
                        if (ErrorMsg.find("name not matched") 
                                                == std::string::npos) {
                            errs() << "phase-bound-pass param parse error: " 
                                                        << ErrorMsg << "\n";
                            return false;
                        }
                    }
                    // Pass name didn't match - let other plugins handle it
                    return false;
                });
        }
    };
}
