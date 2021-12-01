/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * ***** BEGIN LICENSE BLOCK *****
 * Copyright (C) 2008 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * ***** END LICENSE BLOCK ***** */

#ifndef jit_x86_shared_AssemblerBuffer_x86_shared_h
#define jit_x86_shared_AssemblerBuffer_x86_shared_h

#include <stdarg.h>
#include <string.h>

#include "jit/ExecutableAllocator.h"
#include "jit/JitSpewer.h"

// Spew formatting helpers.
#define PRETTYHEX(x)                       (((x)<0)?"-":""),(((x)<0)?-(x):(x))

#define MEM_o     "%s0x%x"
#define MEM_os    MEM_o   "(,%s,%d)"
#define MEM_ob    MEM_o   "(%s)"
#define MEM_obs   MEM_o   "(%s,%s,%d)"

#define MEM_o32   "%s0x%04x"
#define MEM_o32s  MEM_o32 "(,%s,%d)"
#define MEM_o32b  MEM_o32 "(%s)"
#define MEM_o32bs MEM_o32 "(%s,%s,%d)"
#define MEM_o32r  ".Lfrom%d(%%rip)"

#define ADDR_o(offset)                       PRETTYHEX(offset)
#define ADDR_os(offset, index, scale)        ADDR_o(offset), GPRegName((index)), (1<<(scale))
#define ADDR_ob(offset, base)                ADDR_o(offset), GPRegName((base))
#define ADDR_obs(offset, base, index, scale) ADDR_ob(offset, base), GPRegName((index)), (1<<(scale))

#define ADDR_o32(offset)                       ADDR_o(offset)
#define ADDR_o32s(offset, index, scale)        ADDR_os(offset, index, scale)
#define ADDR_o32b(offset, base)                ADDR_ob(offset, base)
#define ADDR_o32bs(offset, base, index, scale) ADDR_obs(offset, base, index, scale)
#define ADDR_o32r(offset)                      (offset)

namespace js {

    class Sprinter;

namespace jit {

    // AllocPolicy for AssemblerBuffer. OOMs when trying to allocate more than
    // MaxCodeBytesPerProcess bytes. Use private inheritance to make sure we
    // explicitly have to expose SystemAllocPolicy methods.
    class AssemblerBufferAllocPolicy : private SystemAllocPolicy
    {
      public:
        using SystemAllocPolicy::checkSimulatedOOM;
        using SystemAllocPolicy::reportAllocOverflow;
        using SystemAllocPolicy::free_;

        template <typename T> T* pod_realloc(T* p, size_t oldSize, size_t newSize) {
            static_assert(sizeof(T) == 1,
                          "AssemblerBufferAllocPolicy should only be used with byte vectors");
            MOZ_ASSERT(oldSize <= MaxCodeBytesPerProcess);
            if (MOZ_UNLIKELY(newSize > MaxCodeBytesPerProcess))
                return nullptr;
            return SystemAllocPolicy::pod_realloc<T>(p, oldSize, newSize);
        }
        template <typename T> T* pod_malloc(size_t numElems) {
            static_assert(sizeof(T) == 1,
                          "AssemblerBufferAllocPolicy should only be used with byte vectors");
            if (MOZ_UNLIKELY(numElems > MaxCodeBytesPerProcess))
                return nullptr;
            return SystemAllocPolicy::pod_malloc<T>(numElems);
        }
    };

    class AssemblerBuffer
    {
        template<size_t size, typename T>
        MOZ_ALWAYS_INLINE void sizedAppendUnchecked(T value)
        {
            m_buffer.infallibleAppend(reinterpret_cast<unsigned char*>(&value), size);
        }

        template<size_t size, typename T>
        MOZ_ALWAYS_INLINE void sizedAppend(T value)
        {
            if (MOZ_UNLIKELY(!m_buffer.append(reinterpret_cast<unsigned char*>(&value), size)))
                oomDetected();
        }

    public:
        AssemblerBuffer() : m_oom(false) {}

        void ensureSpace(size_t space)
        {
            // This should only be called with small |space| values to ensure
            // we don't overflow below.
            MOZ_ASSERT(space <= 16);
            if (MOZ_UNLIKELY(!m_buffer.reserve(m_buffer.length() + space)))
                oomDetected();
        }

        bool isAligned(size_t alignment) const
        {
            return !(m_buffer.length() & (alignment - 1));
        }

        MOZ_ALWAYS_INLINE void putByteUnchecked(int value) { sizedAppendUnchecked<1>(value); }
        MOZ_ALWAYS_INLINE void putShortUnchecked(int value) { sizedAppendUnchecked<2>(value); }
        MOZ_ALWAYS_INLINE void putIntUnchecked(int value) { sizedAppendUnchecked<4>(value); }
        MOZ_ALWAYS_INLINE void putInt64Unchecked(int64_t value) { sizedAppendUnchecked<8>(value); }

        MOZ_ALWAYS_INLINE void putByte(int value) { sizedAppend<1>(value); }
        MOZ_ALWAYS_INLINE void putShort(int value) { sizedAppend<2>(value); }
        MOZ_ALWAYS_INLINE void putInt(int value) { sizedAppend<4>(value); }
        MOZ_ALWAYS_INLINE void putInt64(int64_t value) { sizedAppend<8>(value); }

        MOZ_MUST_USE bool append(const unsigned char* values, size_t size)
        {
            if (MOZ_UNLIKELY(!m_buffer.append(values, size))) {
                oomDetected();
                return false;
            }
            return true;
        }

        size_t size() const
        {
            return m_buffer.length();
        }

        bool oom() const
        {
            return m_oom;
        }

        bool reserve(size_t size)
        {
            return !m_oom && m_buffer.reserve(size);
        }

        bool swap(Vector<uint8_t, 0, SystemAllocPolicy>& bytes);

        const unsigned char* buffer() const
        {
            MOZ_RELEASE_ASSERT(!m_oom);
            return m_buffer.begin();
        }

        unsigned char* data()
        {
            return m_buffer.begin();
        }

    protected:
        /*
         * OOM handling: This class can OOM in the ensureSpace() method trying
         * to allocate a new buffer. In response to an OOM, we need to avoid
         * crashing and report the error. We also want to make it so that
         * users of this class need to check for OOM only at certain points
         * and not after every operation.
         *
         * Our strategy for handling an OOM is to set m_oom, and then clear (but
         * not free) m_buffer, preserving the current buffer. This way, the user
         * can continue assembling into the buffer, deferring OOM checking
         * until the user wants to read code out of the buffer.
         *
         * See also the |buffer| method.
         */
        void oomDetected()
        {
            m_oom = true;
            m_buffer.clear();
        }

        mozilla::Vector<unsigned char, 256, AssemblerBufferAllocPolicy> m_buffer;
        bool m_oom;
    };

    class GenericAssembler
    {
#ifdef JS_JITSPEW
        Sprinter* printer;
#endif
      public:

        GenericAssembler()
#ifdef JS_JITSPEW
          : printer(nullptr)
#endif
        {}

        void setPrinter(Sprinter* sp)
        {
#ifdef JS_JITSPEW
            printer = sp;
#endif
        }

#ifdef JS_JITSPEW
        inline void spew(const char* fmt, ...) MOZ_FORMAT_PRINTF(2, 3)
        {
            if (MOZ_UNLIKELY(printer || JitSpewEnabled(JitSpew_Codegen))) {
                va_list va;
                va_start(va, fmt);
                spew(fmt, va);
                va_end(va);
            }
        }
#else
        MOZ_ALWAYS_INLINE void spew(const char* fmt, ...) MOZ_FORMAT_PRINTF(2, 3)
        { }
#endif

#ifdef JS_JITSPEW
        MOZ_COLD void spew(const char* fmt, va_list va) MOZ_FORMAT_PRINTF(2, 0);
#endif
    };

} // namespace jit
} // namespace js

#endif /* jit_x86_shared_AssemblerBuffer_x86_shared_h */
