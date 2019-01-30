/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2015 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm/WasmCompile.h"

#include "mozilla/Maybe.h"
#include "mozilla/Unused.h"

#include "jit/ProcessExecutableMemory.h"
#include "util/Text.h"
#include "wasm/WasmBaselineCompile.h"
#include "wasm/WasmBinaryIterator.h"
#include "wasm/WasmGenerator.h"
#include "wasm/WasmIonCompile.h"
#include "wasm/WasmSignalHandlers.h"
#include "wasm/WasmValidate.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

template <class DecoderT>
static bool
DecodeFunctionBody(DecoderT& d, ModuleGenerator& mg, uint32_t funcIndex)
{
    uint32_t bodySize;
    if (!d.readVarU32(&bodySize))
        return d.fail("expected number of function body bytes");

    if (bodySize > MaxFunctionBytes)
        return d.fail("function body too big");

    const size_t offsetInModule = d.currentOffset();

    // Skip over the function body; it will be validated by the compilation thread.
    const uint8_t* bodyBegin;
    if (!d.readBytes(bodySize, &bodyBegin))
        return d.fail("function body length too big");

    return mg.compileFuncDef(funcIndex, offsetInModule, bodyBegin, bodyBegin + bodySize);
}

template <class DecoderT>
static bool
DecodeCodeSection(const ModuleEnvironment& env, DecoderT& d, ModuleGenerator& mg)
{
    if (!env.codeSection) {
        if (env.numFuncDefs() != 0)
            return d.fail("expected code section");

        return mg.finishFuncDefs();
    }

    uint32_t numFuncDefs;
    if (!d.readVarU32(&numFuncDefs))
        return d.fail("expected function body count");

    if (numFuncDefs != env.numFuncDefs())
        return d.fail("function body count does not match function signature count");

    for (uint32_t funcDefIndex = 0; funcDefIndex < numFuncDefs; funcDefIndex++) {
        if (!DecodeFunctionBody(d, mg, env.numFuncImports() + funcDefIndex))
            return false;
    }

    if (!d.finishSection(*env.codeSection, "code"))
        return false;

    return mg.finishFuncDefs();
}

bool
CompileArgs::initFromContext(JSContext* cx, ScriptedCaller&& scriptedCaller)
{
    baselineEnabled = cx->options().wasmBaseline();
    ionEnabled = cx->options().wasmIon();
    sharedMemoryEnabled = cx->compartment()->creationOptions().getSharedMemoryAndAtomicsEnabled();
    testTiering = cx->options().testWasmAwaitTier2() || JitOptions.wasmDelayTier2;

    // Debug information such as source view or debug traps will require
    // additional memory and permanently stay in baseline code, so we try to
    // only enable it when a developer actually cares: when the debugger tab
    // is open.
    debugEnabled = cx->compartment()->debuggerObservesAsmJS();

    this->scriptedCaller = Move(scriptedCaller);
    return assumptions.initBuildIdFromContext(cx);
}

// Classify the current system as one of a set of recognizable classes.  This
// really needs to get our tier-1 systems right.
//
// TODO: We don't yet have a good measure of how fast a system is.  We
// distinguish between mobile and desktop because these are very different kinds
// of systems, but we could further distinguish between low / medium / high end
// within those major classes.  If we do so, then constants below would be
// provided for each (class, architecture, system-tier) combination, not just
// (class, architecture) as now.
//
// CPU clock speed is not by itself a good predictor of system performance, as
// there are high-performance systems with slow clocks (recent Intel) and
// low-performance systems with fast clocks (older AMD).  We can also use
// physical memory, core configuration, OS details, CPU class and family, and
// CPU manufacturer to disambiguate.

enum class SystemClass
{
    DesktopX86,
    DesktopX64,
    DesktopUnknown32,
    DesktopUnknown64,
    MobileX86,
    MobileArm32,
    MobileArm64,
    MobileUnknown32,
    MobileUnknown64
};

static SystemClass
ClassifySystem()
{
    bool isDesktop;

#if defined(ANDROID) || defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64)
    isDesktop = false;
#else
    isDesktop = true;
#endif

    if (isDesktop) {
#if defined(JS_CODEGEN_X64)
        return SystemClass::DesktopX64;
#elif defined(JS_CODEGEN_X86)
        return SystemClass::DesktopX86;
#elif defined(JS_64BIT)
        return SystemClass::DesktopUnknown64;
#else
        return SystemClass::DesktopUnknown32;
#endif
    } else {
#if defined(JS_CODEGEN_X86)
        return SystemClass::MobileX86;
#elif defined(JS_CODEGEN_ARM)
        return SystemClass::MobileArm32;
#elif defined(JS_CODEGEN_ARM64)
        return SystemClass::MobileArm64;
#elif defined(JS_64BIT)
        return SystemClass::MobileUnknown64;
#else
        return SystemClass::MobileUnknown32;
#endif
    }
}

// Code sizes in machine code bytes per bytecode byte, again empirical except
// where marked as "Guess".

static const double x64Tox86Inflation = 1.25;

static const double x64IonBytesPerBytecode = 2.45;
static const double x86IonBytesPerBytecode = x64IonBytesPerBytecode * x64Tox86Inflation;
static const double arm32IonBytesPerBytecode = 3.3;
static const double arm64IonBytesPerBytecode = 3.0; // Guess

static const double x64BaselineBytesPerBytecode = x64IonBytesPerBytecode * 1.43;
static const double x86BaselineBytesPerBytecode = x64BaselineBytesPerBytecode * x64Tox86Inflation;
static const double arm32BaselineBytesPerBytecode = arm32IonBytesPerBytecode * 1.39;
static const double arm64BaselineBytesPerBytecode = arm64IonBytesPerBytecode * 1.39; // Guess

static double
IonBytesPerBytecode(SystemClass cls)
{
    switch (cls) {
      case SystemClass::DesktopX86:
      case SystemClass::MobileX86:
      case SystemClass::DesktopUnknown32:
        return x86IonBytesPerBytecode;
      case SystemClass::DesktopX64:
      case SystemClass::DesktopUnknown64:
        return x64IonBytesPerBytecode;
      case SystemClass::MobileArm32:
      case SystemClass::MobileUnknown32:
        return arm32IonBytesPerBytecode;
      case SystemClass::MobileArm64:
      case SystemClass::MobileUnknown64:
        return arm64IonBytesPerBytecode;
      default:
        MOZ_CRASH();
    }
}

static double
BaselineBytesPerBytecode(SystemClass cls)
{
    switch (cls) {
      case SystemClass::DesktopX86:
      case SystemClass::MobileX86:
      case SystemClass::DesktopUnknown32:
        return x86BaselineBytesPerBytecode;
      case SystemClass::DesktopX64:
      case SystemClass::DesktopUnknown64:
        return x64BaselineBytesPerBytecode;
      case SystemClass::MobileArm32:
      case SystemClass::MobileUnknown32:
        return arm32BaselineBytesPerBytecode;
      case SystemClass::MobileArm64:
      case SystemClass::MobileUnknown64:
        return arm64BaselineBytesPerBytecode;
      default:
        MOZ_CRASH();
    }
}

double
wasm::EstimateCompiledCodeSize(Tier tier, size_t bytecodeSize)
{
    SystemClass cls = ClassifySystem();
    switch (tier) {
      case Tier::Baseline:
        return double(bytecodeSize) * BaselineBytesPerBytecode(cls);
      case Tier::Ion:
        return double(bytecodeSize) * IonBytesPerBytecode(cls);
    }
    MOZ_CRASH("bad tier");
}

// If parallel Ion compilation is going to take longer than this, we should tier.

static const double tierCutoffMs = 250;

// Compilation rate values are empirical except when noted, the reference
// systems are:
//
// Late-2013 MacBook Pro (2.6GHz quad hyperthreaded Haswell)
// Late-2015 Nexus 5X (1.4GHz quad Cortex-A53 + 1.8GHz dual Cortex-A57)

static const double x64BytecodesPerMs = 2100;
static const double x86BytecodesPerMs = 1500;
static const double arm32BytecodesPerMs = 450;
static const double arm64BytecodesPerMs = 650; // Guess

// Tiering cutoff values: if code section sizes are below these values (when
// divided by the effective number of cores) we do not tier, because we guess
// that parallel Ion compilation will be fast enough.

static const double x64DesktopTierCutoff = x64BytecodesPerMs * tierCutoffMs;
static const double x86DesktopTierCutoff = x86BytecodesPerMs * tierCutoffMs;
static const double x86MobileTierCutoff = x86DesktopTierCutoff / 2; // Guess
static const double arm32MobileTierCutoff = arm32BytecodesPerMs * tierCutoffMs;
static const double arm64MobileTierCutoff = arm64BytecodesPerMs * tierCutoffMs;

static double
CodesizeCutoff(SystemClass cls, uint32_t codeSize)
{
    switch (cls) {
      case SystemClass::DesktopX86:
      case SystemClass::DesktopUnknown32:
        return x86DesktopTierCutoff;
      case SystemClass::DesktopX64:
      case SystemClass::DesktopUnknown64:
        return x64DesktopTierCutoff;
      case SystemClass::MobileX86:
        return x86MobileTierCutoff;
      case SystemClass::MobileArm32:
      case SystemClass::MobileUnknown32:
        return arm32MobileTierCutoff;
      case SystemClass::MobileArm64:
      case SystemClass::MobileUnknown64:
        return arm64MobileTierCutoff;
      default:
        MOZ_CRASH();
    }
}

// As the number of cores grows the effectiveness of each core dwindles (on the
// systems we care about for SpiderMonkey).
//
// The data are empirical, computed from the observed compilation time of the
// Tanks demo code on a variable number of cores.
//
// The heuristic may fail on NUMA systems where the core count is high but the
// performance increase is nil or negative once the program moves beyond one
// socket.  However, few browser users have such systems.

static double
EffectiveCores(SystemClass cls, uint32_t cores)
{
    if (cores <= 3)
        return pow(cores, 0.9);
    return pow(cores, 0.75);
}

#ifndef JS_64BIT
// Don't tier if tiering will fill code memory to more to more than this
// fraction.

static const double spaceCutoffPct = 0.9;
#endif

// Figure out whether we should use tiered compilation or not.
static bool
TieringBeneficial(uint32_t codeSize)
{
    uint32_t cpuCount = HelperThreadState().cpuCount;
    MOZ_ASSERT(cpuCount > 0);

    // It's mostly sensible not to background compile when there's only one
    // hardware thread as we want foreground computation to have access to that.
    // However, if wasm background compilation helper threads can be given lower
    // priority then background compilation on single-core systems still makes
    // some kind of sense.  That said, this is a non-issue: as of September 2017
    // 1-core was down to 3.5% of our population and falling.

    if (cpuCount == 1)
        return false;

    MOZ_ASSERT(HelperThreadState().threadCount >= cpuCount);

    // Compute the max number of threads available to do actual background
    // compilation work.

    uint32_t workers = HelperThreadState().maxWasmCompilationThreads();

    // The number of cores we will use is bounded both by the CPU count and the
    // worker count.

    uint32_t cores = Min(cpuCount, workers);

    SystemClass cls = ClassifySystem();

    // Ion compilation on available cores must take long enough to be worth the
    // bother.

    double cutoffSize = CodesizeCutoff(cls, codeSize);
    double effectiveCores = EffectiveCores(cls, cores);

    if ((codeSize / effectiveCores) < cutoffSize)
        return false;

    // Do not implement a size cutoff for 64-bit systems since the code size
    // budget for 64 bit is so large that it will hardly ever be an issue.
    // (Also the cutoff percentage might be different on 64-bit.)

#ifndef JS_64BIT
    // If the amount of executable code for baseline compilation jeopardizes the
    // availability of executable memory for ion code then do not tier, for now.
    //
    // TODO: For now we consider this module in isolation.  We should really
    // worry about what else is going on in this process and might be filling up
    // the code memory.  It's like we need some kind of code memory reservation
    // system or JIT compilation for large modules.

    double ionRatio = IonBytesPerBytecode(cls);
    double baselineRatio = BaselineBytesPerBytecode(cls);
    double needMemory = codeSize * (ionRatio + baselineRatio);
    double availMemory = LikelyAvailableExecutableMemory();
    double cutoff = spaceCutoffPct * MaxCodeBytesPerProcess;

    // If the sum of baseline and ion code makes us exceeds some set percentage
    // of the executable memory then disable tiering.

    if ((MaxCodeBytesPerProcess - availMemory) + needMemory > cutoff)
        return false;
#endif

    return true;
}

static void
InitialCompileFlags(const CompileArgs& args, Decoder& d, CompileMode* mode, Tier* tier,
                    DebugEnabled* debug)
{
    uint32_t codeSectionSize = 0;

    SectionRange range;
    if (StartsCodeSection(d.begin(), d.end(), &range))
        codeSectionSize = range.size;

    // Attempt to default to ion if baseline is disabled.
    bool baselineEnabled = BaselineCanCompile() && (args.baselineEnabled || args.testTiering);
    bool debugEnabled = BaselineCanCompile() && args.debugEnabled;
    bool ionEnabled = IonCanCompile() && (args.ionEnabled || !baselineEnabled || args.testTiering);

    // HasCompilerSupport() should prevent failure here
    MOZ_RELEASE_ASSERT(baselineEnabled || ionEnabled);

    if (baselineEnabled && ionEnabled && !debugEnabled && CanUseExtraThreads() &&
        (TieringBeneficial(codeSectionSize) || args.testTiering))
    {
        *mode = CompileMode::Tier1;
        *tier = Tier::Baseline;
    } else {
        *mode = CompileMode::Once;
        *tier = debugEnabled || !ionEnabled ? Tier::Baseline : Tier::Ion;
    }

    *debug = debugEnabled ? DebugEnabled::True : DebugEnabled::False;
}

SharedModule
wasm::CompileBuffer(const CompileArgs& args, const ShareableBytes& bytecode, UniqueChars* error)
{
    MOZ_RELEASE_ASSERT(wasm::HaveSignalHandlers());

    Decoder d(bytecode.bytes, 0, error);

    CompileMode mode;
    Tier tier;
    DebugEnabled debug;
    InitialCompileFlags(args, d, &mode, &tier, &debug);

    ModuleEnvironment env(mode, tier, debug,
                          args.sharedMemoryEnabled ? Shareable::True : Shareable::False);
    if (!DecodeModuleEnvironment(d, &env))
        return nullptr;

    ModuleGenerator mg(args, &env, nullptr, error);
    if (!mg.init())
        return nullptr;

    if (!DecodeCodeSection(env, d, mg))
        return nullptr;

    if (!DecodeModuleTail(d, &env))
        return nullptr;

    return mg.finishModule(bytecode);
}

bool
wasm::CompileTier2(const CompileArgs& args, Module& module, Atomic<bool>* cancelled)
{
    MOZ_RELEASE_ASSERT(wasm::HaveSignalHandlers());

    UniqueChars error;
    Decoder d(module.bytecode().bytes, 0, &error);

    ModuleEnvironment env(CompileMode::Tier2, Tier::Ion, DebugEnabled::False,
                          args.sharedMemoryEnabled ? Shareable::True : Shareable::False);
    if (!DecodeModuleEnvironment(d, &env))
        return false;

    ModuleGenerator mg(args, &env, cancelled, &error);
    if (!mg.init())
        return false;

    if (!DecodeCodeSection(env, d, mg))
        return false;

    if (!DecodeModuleTail(d, &env))
        return false;

    return mg.finishTier2(module);
}

class StreamingDecoder
{
    Decoder d_;
    const ExclusiveStreamEnd& streamEnd_;
    const Atomic<bool>& cancelled_;

  public:
    StreamingDecoder(const ModuleEnvironment& env, const Bytes& begin,
                     const ExclusiveStreamEnd& streamEnd, const Atomic<bool>& cancelled,
                     UniqueChars* error)
      : d_(begin, env.codeSection->start, error),
        streamEnd_(streamEnd),
        cancelled_(cancelled)
    {}

    bool fail(const char* msg) {
        return d_.fail(msg);
    }

    bool done() const {
        return d_.done();
    }

    size_t currentOffset() const {
        return d_.currentOffset();
    }

    bool waitForBytes(size_t numBytes) {
        numBytes = Min(numBytes, d_.bytesRemain());
        const uint8_t* requiredEnd = d_.currentPosition() + numBytes;
        auto streamEnd = streamEnd_.lock();
        while (streamEnd < requiredEnd) {
            if (cancelled_)
                return false;
            streamEnd.wait();
        }
        return true;
    }

    bool readVarU32(uint32_t* u32) {
        return waitForBytes(MaxVarU32DecodedBytes) &&
               d_.readVarU32(u32);
    }

    bool readBytes(size_t size, const uint8_t** begin) {
        return waitForBytes(size) &&
               d_.readBytes(size, begin);
    }

    bool finishSection(const SectionRange& range, const char* name) {
        return d_.finishSection(range, name);
    }
};

static SharedBytes
CreateBytecode(const Bytes& env, const Bytes& code, const Bytes& tail, UniqueChars* error)
{
    size_t size = env.length() + code.length() + tail.length();
    if (size > MaxModuleBytes) {
        *error = DuplicateString("module too big");
        return nullptr;
    }

    MutableBytes bytecode = js_new<ShareableBytes>();
    if (!bytecode || !bytecode->bytes.resize(size))
        return nullptr;

    uint8_t* p = bytecode->bytes.begin();

    memcpy(p, env.begin(), env.length());
    p += env.length();

    memcpy(p, code.begin(), code.length());
    p += code.length();

    memcpy(p, tail.begin(), tail.length());
    p += tail.length();

    MOZ_ASSERT(p == bytecode->end());

    return bytecode;
}

SharedModule
wasm::CompileStreaming(const CompileArgs& args,
                       const Bytes& envBytes,
                       const Bytes& codeBytes,
                       const ExclusiveStreamEnd& codeStreamEnd,
                       const ExclusiveTailBytesPtr& tailBytesPtr,
                       const Atomic<bool>& cancelled,
                       UniqueChars* error)
{
    MOZ_ASSERT(wasm::HaveSignalHandlers());

    Maybe<ModuleEnvironment> env;

    {
        Decoder d(envBytes, 0, error);

        CompileMode mode;
        Tier tier;
        DebugEnabled debug;
        InitialCompileFlags(args, d, &mode, &tier, &debug);

        env.emplace(mode, tier, debug,
                    args.sharedMemoryEnabled ? Shareable::True : Shareable::False);
        if (!DecodeModuleEnvironment(d, env.ptr()))
            return nullptr;

        MOZ_ASSERT(d.done());
    }

    ModuleGenerator mg(args, env.ptr(), &cancelled, error);
    if (!mg.init())
        return nullptr;

    {
        MOZ_ASSERT(env->codeSection->size == codeBytes.length());
        StreamingDecoder d(*env, codeBytes, codeStreamEnd, cancelled, error);

        if (!DecodeCodeSection(*env, d, mg))
            return nullptr;

        MOZ_ASSERT(d.done());
    }

    {
        auto tailBytesPtrGuard = tailBytesPtr.lock();
        while (!tailBytesPtrGuard) {
            if (cancelled)
                return nullptr;
            tailBytesPtrGuard.wait();
        }
    }

    const Bytes& tailBytes = *tailBytesPtr.lock();

    {
        Decoder d(tailBytes, env->codeSection->end(), error);

        if (!DecodeModuleTail(d, env.ptr()))
            return nullptr;

        MOZ_ASSERT(d.done());
    }

    SharedBytes bytecode = CreateBytecode(envBytes, codeBytes, tailBytes, error);
    if (!bytecode)
        return nullptr;

    return mg.finishModule(*bytecode);
}
