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


#ifndef _PHASEBOUNDPASS_HH_
#define _PHASEBOUNDPASS_HH_

#include "common.hh"

const std::vector<Options> PhaseBoundPassOptions = {
    // The BB ID of the warmup marker basic block
    {"warmup_marker_bb_id", ""},
    // Number of executions of the warmup marker basic block before the warmup
    // point is reached
    {"warmup_marker_count", ""},
    // The BB ID of the start marker basic block
    {"start_marker_bb_id", ""},
    // Number of executions of the start marker basic block before the start
    // point is reached
    {"start_marker_count", ""},
    // The BB ID of the end marker basic block
    {"end_marker_bb_id", ""},
    // Number of executions of the end marker basic block before the end
    // point is reached
    {"end_marker_count", ""},
    // If only labeling marker BBs without instrumentation
    {"label_only", "false"},
};

class PhaseBoundPass : public PassInfoMixin<PhaseBoundPass> {
  public:
    PhaseBoundPass(std::vector<Options> Options)
    {
        options_ = Options;
    }
    ~PhaseBoundPass() = default;
  private:
    std::vector<Options> options_;
    bool instrumentMarkerBBs(Module &M,
            const uint64_t warmup_marker_bb_id,
            const uint64_t start_marker_bb_id,
            const uint64_t end_marker_bb_id,
            bool no_warmup_marker);
    bool labelMarkerBBs(Module &M,
          const uint64_t warmup_marker_bb_id,
          const uint64_t start_marker_bb_id,
          const uint64_t end_marker_bb_id,
          bool no_warmup_marker);
  public:
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &);
};

#endif // _PHASEBOUNDPASS_HH_
