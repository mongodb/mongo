// Copyright 2014, ARM Limited
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//   * Redistributions in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//   * Neither the name of ARM Limited nor the names of its contributors may be
//     used to endorse or promote products derived from this software without
//     specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#ifndef VIXL_A64_INSTRUMENT_A64_H_
#define VIXL_A64_INSTRUMENT_A64_H_

#include "mozilla/Vector.h"

#include "jsalloc.h"

#include "jit/arm64/vixl/Constants-vixl.h"
#include "jit/arm64/vixl/Decoder-vixl.h"
#include "jit/arm64/vixl/Globals-vixl.h"
#include "jit/arm64/vixl/Utils-vixl.h"

namespace vixl {

const int kCounterNameMaxLength = 256;
const uint64_t kDefaultInstrumentationSamplingPeriod = 1 << 22;


enum InstrumentState {
  InstrumentStateDisable = 0,
  InstrumentStateEnable = 1
};


enum CounterType {
  Gauge = 0,      // Gauge counters reset themselves after reading.
  Cumulative = 1  // Cumulative counters keep their value after reading.
};


class Counter {
 public:
  explicit Counter(const char* name, CounterType type = Gauge);

  void Increment();
  void Enable();
  void Disable();
  bool IsEnabled();
  uint64_t count();
  const char* name();
  CounterType type();

 private:
  char name_[kCounterNameMaxLength];
  uint64_t count_;
  bool enabled_;
  CounterType type_;
};


class Instrument: public DecoderVisitor {
 public:
  explicit Instrument(const char* datafile = NULL,
    uint64_t sample_period = kDefaultInstrumentationSamplingPeriod);
  ~Instrument();

  void Enable();
  void Disable();

  // Declare all Visitor functions.
  #define DECLARE(A) void Visit##A(const Instruction* instr);
  VISITOR_LIST(DECLARE)
  #undef DECLARE

 private:
  void Update();
  void DumpCounters();
  void DumpCounterNames();
  void DumpEventMarker(unsigned marker);
  void HandleInstrumentationEvent(unsigned event);
  Counter* GetCounter(const char* name);

  void InstrumentLoadStore(const Instruction* instr);
  void InstrumentLoadStorePair(const Instruction* instr);

  mozilla::Vector<Counter*, 8, js::SystemAllocPolicy> counters_;

  FILE *output_stream_;
  uint64_t sample_period_;
};

}  // namespace vixl

#endif  // VIXL_A64_INSTRUMENT_A64_H_
