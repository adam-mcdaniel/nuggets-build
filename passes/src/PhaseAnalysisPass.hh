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


#ifndef _PHASEANALYSISPASS_HH_
#define _PHASEANALYSISPASS_HH_

#include "common.hh"

const std::vector<Options> PhaseAnalysisPassOptions = {
    {"interval_length", ""}, // Length in terms of IR instruction executed
};

// PhaseAnalysisPass - instrument every basic block to collect runtime data
// It expects every IR basic block is labeled with metadata from IRBBLabel pass
// This is important because it ensures the IR basic block identify remains
// stable.

class PhaseAnalysisPass : public PassInfoMixin<PhaseAnalysisPass> {
  public:
    PhaseAnalysisPass(std::vector<Options> Options)
    {
        options_ = Options;
    }
    ~PhaseAnalysisPass() = default;
  private:
    std::vector<Options> options_;
    bool instrumentAllIRBasicBlocks(Module &M, 
                  int64_t &total_basic_block_count, const uint64_t threshold);
  
  public:
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &);
};

#endif // _PHASEANALYSISPASS_HH_