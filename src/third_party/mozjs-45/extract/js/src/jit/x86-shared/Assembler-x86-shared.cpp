/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "gc/Marking.h"
#include "jit/Disassembler.h"
#include "jit/JitCompartment.h"
#if defined(JS_CODEGEN_X86)
# include "jit/x86/MacroAssembler-x86.h"
#elif defined(JS_CODEGEN_X64)
# include "jit/x64/MacroAssembler-x64.h"
#else
# error "Wrong architecture. Only x86 and x64 should build this file!"
#endif

#ifdef _MSC_VER
# include <intrin.h> // for __cpuid
# if defined(_M_X64) && (_MSC_FULL_VER >= 160040219)
#  include <immintrin.h> // for _xgetbv
# endif
#endif

using namespace js;
using namespace js::jit;

void
AssemblerX86Shared::copyJumpRelocationTable(uint8_t* dest)
{
    if (jumpRelocations_.length())
        memcpy(dest, jumpRelocations_.buffer(), jumpRelocations_.length());
}

void
AssemblerX86Shared::copyDataRelocationTable(uint8_t* dest)
{
    if (dataRelocations_.length())
        memcpy(dest, dataRelocations_.buffer(), dataRelocations_.length());
}

void
AssemblerX86Shared::copyPreBarrierTable(uint8_t* dest)
{
    if (preBarriers_.length())
        memcpy(dest, preBarriers_.buffer(), preBarriers_.length());
}

static void
TraceDataRelocations(JSTracer* trc, uint8_t* buffer, CompactBufferReader& reader)
{
    while (reader.more()) {
        size_t offset = reader.readUnsigned();
        void** ptr = X86Encoding::GetPointerRef(buffer + offset);

#ifdef JS_PUNBOX64
        // All pointers on x64 will have the top bits cleared. If those bits
        // are not cleared, this must be a Value.
        uintptr_t* word = reinterpret_cast<uintptr_t*>(ptr);
        if (*word >> JSVAL_TAG_SHIFT) {
            jsval_layout layout;
            layout.asBits = *word;
            Value v = IMPL_TO_JSVAL(layout);
            TraceManuallyBarrieredEdge(trc, &v, "ion-masm-value");
            if (*word != JSVAL_TO_IMPL(v).asBits)
                *word = JSVAL_TO_IMPL(v).asBits;
            continue;
        }
#endif

        // No barrier needed since these are constants.
        TraceManuallyBarrieredGenericPointerEdge(trc, reinterpret_cast<gc::Cell**>(ptr),
                                                 "ion-masm-ptr");
    }
}


void
AssemblerX86Shared::TraceDataRelocations(JSTracer* trc, JitCode* code, CompactBufferReader& reader)
{
    ::TraceDataRelocations(trc, code->raw(), reader);
}

void
AssemblerX86Shared::trace(JSTracer* trc)
{
    for (size_t i = 0; i < jumps_.length(); i++) {
        RelativePatch& rp = jumps_[i];
        if (rp.kind == Relocation::JITCODE) {
            JitCode* code = JitCode::FromExecutable((uint8_t*)rp.target);
            TraceManuallyBarrieredEdge(trc, &code, "masmrel32");
            MOZ_ASSERT(code == JitCode::FromExecutable((uint8_t*)rp.target));
        }
    }
    if (dataRelocations_.length()) {
        CompactBufferReader reader(dataRelocations_);
        ::TraceDataRelocations(trc, masm.data(), reader);
    }
}

void
AssemblerX86Shared::executableCopy(void* buffer)
{
    masm.executableCopy(buffer);
}

void
AssemblerX86Shared::processCodeLabels(uint8_t* rawCode)
{
    for (size_t i = 0; i < codeLabels_.length(); i++) {
        CodeLabel label = codeLabels_[i];
        Bind(rawCode, label.patchAt(), rawCode + label.target()->offset());
    }
}

AssemblerX86Shared::Condition
AssemblerX86Shared::InvertCondition(Condition cond)
{
    switch (cond) {
      case Zero:
        return NonZero;
      case NonZero:
        return Zero;
      case LessThan:
        return GreaterThanOrEqual;
      case LessThanOrEqual:
        return GreaterThan;
      case GreaterThan:
        return LessThanOrEqual;
      case GreaterThanOrEqual:
        return LessThan;
      case Above:
        return BelowOrEqual;
      case AboveOrEqual:
        return Below;
      case Below:
        return AboveOrEqual;
      case BelowOrEqual:
        return Above;
      default:
        MOZ_CRASH("unexpected condition");
    }
}

void
AssemblerX86Shared::verifyHeapAccessDisassembly(uint32_t begin, uint32_t end,
                                                const Disassembler::HeapAccess& heapAccess)
{
#ifdef DEBUG
    Disassembler::VerifyHeapAccess(masm.data() + begin, masm.data() + end, heapAccess);
#endif
}

CPUInfo::SSEVersion CPUInfo::maxSSEVersion = UnknownSSE;
CPUInfo::SSEVersion CPUInfo::maxEnabledSSEVersion = UnknownSSE;
bool CPUInfo::avxPresent = false;
bool CPUInfo::avxEnabled = false;

static uintptr_t
ReadXGETBV()
{
    // We use a variety of low-level mechanisms to get at the xgetbv
    // instruction, including spelling out the xgetbv instruction as bytes,
    // because older compilers and assemblers may not recognize the instruction
    // by name.
    size_t xcr0EAX = 0;
#if defined(_XCR_XFEATURE_ENABLED_MASK)
    xcr0EAX = _xgetbv(_XCR_XFEATURE_ENABLED_MASK);
#elif defined(__GNUC__)
    // xgetbv returns its results in %eax and %edx, and for our purposes here,
    // we're only interested in the %eax value.
    asm(".byte 0x0f, 0x01, 0xd0" : "=a"(xcr0EAX) : "c"(0) : "%edx");
#elif defined(_MSC_VER) && defined(_M_IX86)
    __asm {
        xor ecx, ecx
        _asm _emit 0x0f _asm _emit 0x01 _asm _emit 0xd0
        mov xcr0EAX, eax
    }
#endif
    return xcr0EAX;
}

void
CPUInfo::SetSSEVersion()
{
    int flagsEDX = 0;
    int flagsECX = 0;

#ifdef _MSC_VER
    int cpuinfo[4];
    __cpuid(cpuinfo, 1);
    flagsECX = cpuinfo[2];
    flagsEDX = cpuinfo[3];
#elif defined(__GNUC__)
# ifdef JS_CODEGEN_X64
    asm (
         "movl $0x1, %%eax;"
         "cpuid;"
         : "=c" (flagsECX), "=d" (flagsEDX)
         :
         : "%eax", "%ebx"
         );
# else
    // On x86, preserve ebx. The compiler needs it for PIC mode.
    // Some older processors don't fill the ecx register with cpuid, so clobber
    // it before calling cpuid, so that there's no risk of picking random bits
    // indicating SSE3/SSE4 are present.
    asm (
         "xor %%ecx, %%ecx;"
         "movl $0x1, %%eax;"
         "pushl %%ebx;"
         "cpuid;"
         "popl %%ebx;"
         : "=c" (flagsECX), "=d" (flagsEDX)
         :
         : "%eax"
         );
# endif
#else
# error "Unsupported compiler"
#endif

    static const int SSEBit = 1 << 25;
    static const int SSE2Bit = 1 << 26;
    static const int SSE3Bit = 1 << 0;
    static const int SSSE3Bit = 1 << 9;
    static const int SSE41Bit = 1 << 19;
    static const int SSE42Bit = 1 << 20;

    if (flagsECX & SSE42Bit)      maxSSEVersion = SSE4_2;
    else if (flagsECX & SSE41Bit) maxSSEVersion = SSE4_1;
    else if (flagsECX & SSSE3Bit) maxSSEVersion = SSSE3;
    else if (flagsECX & SSE3Bit)  maxSSEVersion = SSE3;
    else if (flagsEDX & SSE2Bit)  maxSSEVersion = SSE2;
    else if (flagsEDX & SSEBit)   maxSSEVersion = SSE;
    else                          maxSSEVersion = NoSSE;

    if (maxEnabledSSEVersion != UnknownSSE)
        maxSSEVersion = Min(maxSSEVersion, maxEnabledSSEVersion);

    static const int AVXBit = 1 << 28;
    static const int XSAVEBit = 1 << 27;
    avxPresent = (flagsECX & AVXBit) && (flagsECX & XSAVEBit) && avxEnabled;

    // If the hardware supports AVX, check whether the OS supports it too.
    if (avxPresent) {
        size_t xcr0EAX = ReadXGETBV();
        static const int xcr0SSEBit = 1 << 1;
        static const int xcr0AVXBit = 1 << 2;
        avxPresent = (xcr0EAX & xcr0SSEBit) && (xcr0EAX & xcr0AVXBit);
    }
}
