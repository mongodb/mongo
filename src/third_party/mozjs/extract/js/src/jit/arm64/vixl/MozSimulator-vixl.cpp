// Copyright 2013, ARM Limited
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

#include "mozilla/DebugOnly.h"

#include "jit/arm64/vixl/Debugger-vixl.h"
#include "jit/arm64/vixl/MozCachingDecoder.h"
#include "jit/arm64/vixl/Simulator-vixl.h"
#include "jit/IonTypes.h"
#include "js/UniquePtr.h"
#include "js/Utility.h"
#include "threading/LockGuard.h"
#include "vm/JSContext.h"
#include "vm/Runtime.h"

js::jit::SimulatorProcess* js::jit::SimulatorProcess::singleton_ = nullptr;

namespace vixl {

using mozilla::DebugOnly;
using js::jit::ABIFunctionType;
using js::jit::JitActivation;
using js::jit::SimulatorProcess;

Simulator::Simulator(Decoder* decoder, FILE* stream)
  : stream_(nullptr)
  , print_disasm_(nullptr)
  , instrumentation_(nullptr)
  , stack_(nullptr)
  , stack_limit_(nullptr)
  , decoder_(nullptr)
  , oom_(false)
{
    this->init(decoder, stream);

    // If this environment variable is present, trace the executed instructions.
    // (Very helpful for debugging code generation crashes.)
    if (getenv("VIXL_TRACE")) {
        set_trace_parameters(LOG_DISASM);
    }
}


Simulator::~Simulator() {
  js_free(stack_);
  stack_ = nullptr;

  // The decoder may outlive the simulator.
  if (print_disasm_) {
    decoder_->RemoveVisitor(print_disasm_);
    js_delete(print_disasm_);
    print_disasm_ = nullptr;
  }

  if (instrumentation_) {
    decoder_->RemoveVisitor(instrumentation_);
    js_delete(instrumentation_);
    instrumentation_ = nullptr;
  }
}


void Simulator::ResetState() {
  // Reset the system registers.
  nzcv_ = SimSystemRegister::DefaultValueFor(NZCV);
  fpcr_ = SimSystemRegister::DefaultValueFor(FPCR);

  // Reset registers to 0.
  pc_ = nullptr;
  pc_modified_ = false;
  for (unsigned i = 0; i < kNumberOfRegisters; i++) {
    set_xreg(i, 0xbadbeef);
  }
  // Set FP registers to a value that is a NaN in both 32-bit and 64-bit FP.
  uint64_t nan_bits = UINT64_C(0x7ff0dead7f8beef1);
  VIXL_ASSERT(IsSignallingNaN(RawbitsToDouble(nan_bits & kDRegMask)));
  VIXL_ASSERT(IsSignallingNaN(RawbitsToFloat(nan_bits & kSRegMask)));
  for (unsigned i = 0; i < kNumberOfFPRegisters; i++) {
    set_dreg_bits(i, nan_bits);
  }
  // Returning to address 0 exits the Simulator.
  set_lr(kEndOfSimAddress);
}


void Simulator::init(Decoder* decoder, FILE* stream) {
  // Ensure that shift operations act as the simulator expects.
  VIXL_ASSERT((static_cast<int32_t>(-1) >> 1) == -1);
  VIXL_ASSERT((static_cast<uint32_t>(-1) >> 1) == 0x7FFFFFFF);

  instruction_stats_ = false;

  // Set up the decoder.
  decoder_ = decoder;
  decoder_->AppendVisitor(this);

  stream_ = stream;
  print_disasm_ = js_new<PrintDisassembler>(stream_);
  if (!print_disasm_) {
    oom_ = true;
    return;
  }
  set_coloured_trace(false);
  trace_parameters_ = LOG_NONE;

  ResetState();

  // Allocate and set up the simulator stack.
  stack_ = js_pod_malloc<byte>(stack_size_);
  if (!stack_) {
    oom_ = true;
    return;
  }
  stack_limit_ = stack_ + stack_protection_size_;
  // Configure the starting stack pointer.
  //  - Find the top of the stack.
  byte * tos = stack_ + stack_size_;
  //  - There's a protection region at both ends of the stack.
  tos -= stack_protection_size_;
  //  - The stack pointer must be 16-byte aligned.
  tos = AlignDown(tos, 16);
  set_sp(tos);

  // Set the sample period to 10, as the VIXL examples and tests are short.
  if (getenv("VIXL_STATS")) {
    instrumentation_ = js_new<Instrument>("vixl_stats.csv", 10);
    if (!instrumentation_) {
      oom_ = true;
      return;
    }
  }

  // Print a warning about exclusive-access instructions, but only the first
  // time they are encountered. This warning can be silenced using
  // SilenceExclusiveAccessWarning().
  print_exclusive_access_warning_ = true;
}


Simulator* Simulator::Current() {
  JSContext* cx = js::TlsContext.get();
  if (!cx) {
    return nullptr;
  }
  JSRuntime* rt = cx->runtime();
  if (!rt) {
    return nullptr;
  }
  if (!js::CurrentThreadCanAccessRuntime(rt)) {
      return nullptr;
  }
  return cx->simulator();
}


Simulator* Simulator::Create() {
  Decoder *decoder = js_new<Decoder>();
  if (!decoder)
    return nullptr;

  // FIXME: This just leaks the Decoder object for now, which is probably OK.
  // FIXME: We should free it at some point.
  // FIXME: Note that it can't be stored in the SimulatorRuntime due to lifetime conflicts.
  js::UniquePtr<Simulator> sim;
  if (getenv("USE_DEBUGGER") != nullptr) {
    sim.reset(js_new<Debugger>(decoder, stdout));
  } else {
    sim.reset(js_new<Simulator>(decoder, stdout));
  }

  // Check if Simulator:init ran out of memory.
  if (sim && sim->oom()) {
    return nullptr;
  }

#ifdef JS_CACHE_SIMULATOR_ARM64
  // Register the simulator in the Simulator process to handle cache flushes
  // across threads.
  js::jit::AutoLockSimulatorCache alsc;
  if (!SimulatorProcess::registerSimulator(sim.get())) {
    return nullptr;
  }
#endif

  return sim.release();
}


void Simulator::Destroy(Simulator* sim) {
#ifdef JS_CACHE_SIMULATOR_ARM64
  if (sim) {
    js::jit::AutoLockSimulatorCache alsc;
    SimulatorProcess::unregisterSimulator(sim);
  }
#endif

  js_delete(sim);
}


void Simulator::ExecuteInstruction() {
  // The program counter should always be aligned.
  VIXL_ASSERT(IsWordAligned(pc_));
#ifdef JS_CACHE_SIMULATOR_ARM64
  if (pendingCacheRequests) {
      // We're here emulating the behavior of the membarrier carried over on
      // real hardware does; see syscalls to membarrier in MozCpu-vixl.cpp.
      // There's a slight difference that the simulator is not being
      // interrupted: instead, we effectively run the icache flush request
      // before executing the next instruction, which is close enough and
      // sufficient for our use case.
      js::jit::AutoLockSimulatorCache alsc;
      FlushICache();
  }
#endif
  decoder_->Decode(pc_);
  increment_pc();
}


uintptr_t Simulator::stackLimit() const {
  return reinterpret_cast<uintptr_t>(stack_limit_);
}


uintptr_t* Simulator::addressOfStackLimit() {
  return (uintptr_t*)&stack_limit_;
}


bool Simulator::overRecursed(uintptr_t newsp) const {
  if (newsp == 0) {
    newsp = get_sp();
  }
  return newsp <= stackLimit();
}


bool Simulator::overRecursedWithExtra(uint32_t extra) const {
  uintptr_t newsp = get_sp() - extra;
  return newsp <= stackLimit();
}


JS::ProfilingFrameIterator::RegisterState
Simulator::registerState()
{
  JS::ProfilingFrameIterator::RegisterState state;
  state.pc = (uint8_t*) get_pc();
  state.fp = (uint8_t*) get_fp();
  state.lr = (uint8_t*) get_lr();
  state.sp = (uint8_t*) get_sp();
  return state;
}

int64_t Simulator::call(uint8_t* entry, int argument_count, ...) {
  va_list parameters;
  va_start(parameters, argument_count);

  // First eight arguments passed in registers.
  VIXL_ASSERT(argument_count <= 8);
  // This code should use the type of the called function
  // (with templates, like the callVM machinery), but since the
  // number of called functions is miniscule, their types have been
  // divined from the number of arguments.
  if (argument_count == 8) {
    // EnterJitData::jitcode.
    set_xreg(0, va_arg(parameters, int64_t));
    // EnterJitData::maxArgc.
    set_xreg(1, va_arg(parameters, unsigned));
    // EnterJitData::maxArgv.
    set_xreg(2, va_arg(parameters, int64_t));
    // EnterJitData::osrFrame.
    set_xreg(3, va_arg(parameters, int64_t));
    // EnterJitData::calleeToken.
    set_xreg(4, va_arg(parameters, int64_t));
    // EnterJitData::scopeChain.
    set_xreg(5, va_arg(parameters, int64_t));
    // EnterJitData::osrNumStackValues.
    set_xreg(6, va_arg(parameters, unsigned));
    // Address of EnterJitData::result.
    set_xreg(7, va_arg(parameters, int64_t));
  } else if (argument_count == 2) {
    // EntryArg* args
    set_xreg(0, va_arg(parameters, int64_t));
    // uint8_t* GlobalData
    set_xreg(1, va_arg(parameters, int64_t));
  } else if (argument_count == 1) { // irregexp
    // InputOutputData& data
    set_xreg(0, va_arg(parameters, int64_t));
  } else if (argument_count == 0) { // testsJit.cpp
    // accept.
  } else {
    MOZ_CRASH("Unknown number of arguments");
  }

  va_end(parameters);

  // Call must transition back to native code on exit.
  VIXL_ASSERT(get_lr() == int64_t(kEndOfSimAddress));

  // Execute the simulation.
  DebugOnly<int64_t> entryStack = get_sp();
  RunFrom((Instruction*)entry);
  DebugOnly<int64_t> exitStack = get_sp();
  VIXL_ASSERT(entryStack == exitStack);

  int64_t result = xreg(0);
  if (getenv("USE_DEBUGGER")) {
    printf("LEAVE\n");
  }
  return result;
}


// When the generated code calls a VM function (masm.callWithABI) we need to
// call that function instead of trying to execute it with the simulator
// (because it's x64 code instead of AArch64 code). We do that by redirecting the VM
// call to a svc (Supervisor Call) instruction that is handled by the
// simulator. We write the original destination of the jump just at a known
// offset from the svc instruction so the simulator knows what to call.
class Redirection
{
  friend class Simulator;

  Redirection(void* nativeFunction, ABIFunctionType type)
    : nativeFunction_(nativeFunction),
    type_(type),
    next_(nullptr)
  {
    next_ = SimulatorProcess::redirection();
    SimulatorProcess::setRedirection(this);

    Instruction* instr = (Instruction*)(&svcInstruction_);
    vixl::Assembler::svc(instr, kCallRtRedirected);
  }

 public:
  void* addressOfSvcInstruction() { return &svcInstruction_; }
  void* nativeFunction() const { return nativeFunction_; }
  ABIFunctionType type() const { return type_; }

  static Redirection* Get(void* nativeFunction, ABIFunctionType type) {
    js::jit::AutoLockSimulatorCache alsr;

    // TODO: Store srt_ in the simulator for this assertion.
    // VIXL_ASSERT_IF(pt->simulator(), pt->simulator()->srt_ == srt);

    Redirection* current = SimulatorProcess::redirection();
    for (; current != nullptr; current = current->next_) {
      if (current->nativeFunction_ == nativeFunction) {
        VIXL_ASSERT(current->type() == type);
        return current;
      }
    }

    // Note: we can't use js_new here because the constructor is private.
    js::AutoEnterOOMUnsafeRegion oomUnsafe;
    Redirection* redir = js_pod_malloc<Redirection>(1);
    if (!redir)
        oomUnsafe.crash("Simulator redirection");
    new(redir) Redirection(nativeFunction, type);
    return redir;
  }

  static const Redirection* FromSvcInstruction(const Instruction* svcInstruction) {
    const uint8_t* addrOfSvc = reinterpret_cast<const uint8_t*>(svcInstruction);
    const uint8_t* addrOfRedirection = addrOfSvc - offsetof(Redirection, svcInstruction_);
    return reinterpret_cast<const Redirection*>(addrOfRedirection);
  }

 private:
  void* nativeFunction_;
  uint32_t svcInstruction_;
  ABIFunctionType type_;
  Redirection* next_;
};




void* Simulator::RedirectNativeFunction(void* nativeFunction, ABIFunctionType type) {
  Redirection* redirection = Redirection::Get(nativeFunction, type);
  return redirection->addressOfSvcInstruction();
}

void Simulator::VisitException(const Instruction* instr) {
  if (instr->InstructionBits() == UNDEFINED_INST_PATTERN) {
    uint8_t* newPC;
    if (js::wasm::HandleIllegalInstruction(registerState(), &newPC)) {
      set_pc((Instruction*)newPC);
      return;
    }
    DoUnreachable(instr);
  }

  switch (instr->Mask(ExceptionMask)) {
    case BRK: {
      int lowbit  = ImmException_offset;
      int highbit = ImmException_offset + ImmException_width - 1;
      HostBreakpoint(instr->Bits(highbit, lowbit));
      break;
    }
    case HLT:
      switch (instr->ImmException()) {
        case kTraceOpcode:
          DoTrace(instr);
          return;
        case kLogOpcode:
          DoLog(instr);
          return;
        case kPrintfOpcode:
          DoPrintf(instr);
          return;
        default:
          HostBreakpoint();
          return;
      }
    case SVC:
      // The SVC instruction is hijacked by the JIT as a pseudo-instruction
      // causing the Simulator to execute host-native code for callWithABI.
      switch (instr->ImmException()) {
        case kCallRtRedirected:
          VisitCallRedirection(instr);
          return;
        case kMarkStackPointer: {
          js::AutoEnterOOMUnsafeRegion oomUnsafe;
          if (!spStack_.append(get_sp()))
            oomUnsafe.crash("tracking stack for ARM64 simulator");
          return;
        }
        case kCheckStackPointer: {
          DebugOnly<int64_t> current = get_sp();
          DebugOnly<int64_t> expected = spStack_.popCopy();
          VIXL_ASSERT(current == expected);
          return;
        }
        default:
          VIXL_UNIMPLEMENTED();
      }
      break;
    default:
      VIXL_UNIMPLEMENTED();
  }
}


void Simulator::setGPR32Result(int32_t result) {
    set_wreg(0, result);
}


void Simulator::setGPR64Result(int64_t result) {
    set_xreg(0, result);
}


void Simulator::setFP32Result(float result) {
    set_sreg(0, result);
}


void Simulator::setFP64Result(double result) {
    set_dreg(0, result);
}

ABI_FUNCTION_TYPE_SIM_PROTOTYPES

// Simulator support for callWithABI().
void
Simulator::VisitCallRedirection(const Instruction* instr)
{
  VIXL_ASSERT(instr->Mask(ExceptionMask) == SVC);
  VIXL_ASSERT(instr->ImmException() == kCallRtRedirected);

  const Redirection* redir = Redirection::FromSvcInstruction(instr);
  uintptr_t nativeFn = reinterpret_cast<uintptr_t>(redir->nativeFunction());

  // Stack must be aligned prior to the call.
  // FIXME: It's actually our job to perform the alignment...
  //VIXL_ASSERT((xreg(31, Reg31IsStackPointer) & (StackAlignment - 1)) == 0);

  // Used to assert that callee-saved registers are preserved.
  DebugOnly<int64_t> x19 = xreg(19);
  DebugOnly<int64_t> x20 = xreg(20);
  DebugOnly<int64_t> x21 = xreg(21);
  DebugOnly<int64_t> x22 = xreg(22);
  DebugOnly<int64_t> x23 = xreg(23);
  DebugOnly<int64_t> x24 = xreg(24);
  DebugOnly<int64_t> x25 = xreg(25);
  DebugOnly<int64_t> x26 = xreg(26);
  DebugOnly<int64_t> x27 = xreg(27);
  DebugOnly<int64_t> x28 = xreg(28);
  DebugOnly<int64_t> x29 = xreg(29);
  DebugOnly<int64_t> savedSP = get_sp();

  // Get the SP for reading stack arguments
  int64_t* sp = reinterpret_cast<int64_t*>(get_sp());
  // Remember LR for returning from the "call".
  int64_t savedLR = xreg(30);

  // Allow recursive Simulator calls: returning from the call must stop
  // the simulation and transition back to native Simulator code.
  set_xreg(30, int64_t(kEndOfSimAddress));

  // Store argument register values in local variables for ease of use below.
  int64_t x0 = xreg(0);
  int64_t x1 = xreg(1);
  int64_t x2 = xreg(2);
  int64_t x3 = xreg(3);
  int64_t x4 = xreg(4);
  int64_t x5 = xreg(5);
  int64_t x6 = xreg(6);
  int64_t x7 = xreg(7);
  double d0 = dreg(0);
  double d1 = dreg(1);
  double d2 = dreg(2);
  double d3 = dreg(3);
  float s0 = sreg(0);
  float s1 = sreg(1);
  float s2 = sreg(2);
  float s3 = sreg(3);
  float s4 = sreg(4);

  // Dispatch the call and set the return value.
  switch (redir->type()) {
    ABI_FUNCTION_TYPE_ARM64_SIM_DISPATCH

    default:
      MOZ_CRASH("Unknown function type.");
  }

  // Nuke the volatile registers. x0-x7 are used as result registers, but except
  // for x0, none are used in the above signatures.
  for (int i = 1; i <= 18; i++) {
    // Code feed 1 bad data
    set_xreg(i, int64_t(0xc0defeed1badda7a));
  }

  // Assert that callee-saved registers are unchanged.
  VIXL_ASSERT(xreg(19) == x19);
  VIXL_ASSERT(xreg(20) == x20);
  VIXL_ASSERT(xreg(21) == x21);
  VIXL_ASSERT(xreg(22) == x22);
  VIXL_ASSERT(xreg(23) == x23);
  VIXL_ASSERT(xreg(24) == x24);
  VIXL_ASSERT(xreg(25) == x25);
  VIXL_ASSERT(xreg(26) == x26);
  VIXL_ASSERT(xreg(27) == x27);
  VIXL_ASSERT(xreg(28) == x28);
  VIXL_ASSERT(xreg(29) == x29);

  // Assert that the stack is unchanged.
  VIXL_ASSERT(savedSP == get_sp());

  // Simulate a return.
  set_lr(savedLR);
  set_pc((Instruction*)savedLR);
  if (getenv("USE_DEBUGGER"))
    printf("SVCRET\n");
}

#ifdef JS_CACHE_SIMULATOR_ARM64
void
Simulator::FlushICache()
{
  // Flush the caches recorded by the current thread as well as what got
  // recorded from other threads before this call.
  auto& vec = SimulatorProcess::getICacheFlushes(this);
  for (auto& flush : vec) {
    decoder_->FlushICache(flush.start, flush.length);
  }
  vec.clear();
  pendingCacheRequests = false;
}

void CachingDecoder::Decode(const Instruction* instr) {
  InstDecodedKind state;
  if (lastPage_ && lastPage_->contains(instr)) {
    state = lastPage_->decode(instr);
  } else {
    uintptr_t key = SinglePageDecodeCache::PageStart(instr);
    ICacheMap::AddPtr p = iCache_.lookupForAdd(key);
    if (p) {
      lastPage_ = p->value();
      state = lastPage_->decode(instr);
    } else {
      js::AutoEnterOOMUnsafeRegion oomUnsafe;
      SinglePageDecodeCache* newPage = js_new<SinglePageDecodeCache>(instr);
      if (!newPage || !iCache_.add(p, key, newPage)) {
        oomUnsafe.crash("Simulator SinglePageDecodeCache");
      }
      lastPage_ = newPage;
      state = InstDecodedKind::NotDecodedYet;
    }
  }

  switch (state) {
  case InstDecodedKind::NotDecodedYet: {
    cachingDecoder_.setDecodePtr(lastPage_->decodePtr(instr));
    this->Decoder::Decode(instr);
    break;
  }
#define CASE(A) \
  case InstDecodedKind::A: { \
    Visit##A(instr); \
    break; \
  }

  VISITOR_LIST(CASE)
#undef CASE
  }
}

void CachingDecoder::FlushICache(void* start, size_t size) {
  MOZ_ASSERT(uintptr_t(start) % vixl::kInstructionSize == 0);
  MOZ_ASSERT(size % vixl::kInstructionSize == 0);
  const uint8_t* it = reinterpret_cast<const uint8_t*>(start);
  const uint8_t* end = it + size;
  SinglePageDecodeCache* last = nullptr;
  for (; it < end; it += vixl::kInstructionSize) {
    auto instr = reinterpret_cast<const Instruction*>(it);
    if (last && last->contains(instr)) {
      last->clearDecode(instr);
    } else {
      uintptr_t key = SinglePageDecodeCache::PageStart(instr);
      ICacheMap::Ptr p = iCache_.lookup(key);
      if (p) {
        last = p->value();
        last->clearDecode(instr);
      }
    }
  }
}
#endif

}  // namespace vixl

namespace js {
namespace jit {

#ifdef JS_CACHE_SIMULATOR_ARM64
void SimulatorProcess::recordICacheFlush(void* start, size_t length) {
  singleton_->lock_.assertOwnedByCurrentThread();
  AutoEnterOOMUnsafeRegion oomUnsafe;
  ICacheFlush range{start, length};
  for (auto& s : singleton_->pendingFlushes_) {
    if (!s.records.append(range)) {
      oomUnsafe.crash("Simulator recordFlushICache");
    }
  }
}

void SimulatorProcess::membarrier() {
  singleton_->lock_.assertOwnedByCurrentThread();
  for (auto& s : singleton_->pendingFlushes_) {
    s.thread->pendingCacheRequests = true;
  }
}

SimulatorProcess::ICacheFlushes& SimulatorProcess::getICacheFlushes(Simulator* sim) {
  singleton_->lock_.assertOwnedByCurrentThread();
  for (auto& s : singleton_->pendingFlushes_) {
    if (s.thread == sim) {
      return s.records;
    }
  }
  MOZ_CRASH("Simulator is not registered in the SimulatorProcess");
}

bool SimulatorProcess::registerSimulator(Simulator* sim) {
  singleton_->lock_.assertOwnedByCurrentThread();
  ICacheFlushes empty;
  SimFlushes simFlushes{sim, std::move(empty)};
  return singleton_->pendingFlushes_.append(std::move(simFlushes));
}

void SimulatorProcess::unregisterSimulator(Simulator* sim) {
  singleton_->lock_.assertOwnedByCurrentThread();
  for (auto& s : singleton_->pendingFlushes_) {
    if (s.thread == sim) {
      singleton_->pendingFlushes_.erase(&s);
      return;
    }
  }
  MOZ_CRASH("Simulator is not registered in the SimulatorProcess");
}
#endif // !JS_CACHE_SIMULATOR_ARM64

} // namespace jit
} // namespace js

vixl::Simulator* JSContext::simulator() const {
  return simulator_;
}
