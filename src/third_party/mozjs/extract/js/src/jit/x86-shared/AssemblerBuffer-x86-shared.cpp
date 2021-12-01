/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "jit/x86-shared/AssemblerBuffer-x86-shared.h"

#include "mozilla/Sprintf.h"

#include "vm/BytecodeUtil.h"

using namespace js;
using namespace jit;

bool
AssemblerBuffer::swap(Vector<uint8_t, 0, SystemAllocPolicy>& bytes)
{
    // For now, specialize to the one use case.
    MOZ_ASSERT(bytes.empty());

    if (m_buffer.empty()) {
        if (bytes.capacity() > m_buffer.capacity()) {
            size_t newCapacity = bytes.capacity();
            uint8_t* newBuffer = bytes.extractRawBuffer();
            MOZ_ASSERT(newBuffer);
            m_buffer.replaceRawBuffer((unsigned char*)newBuffer, 0, newCapacity);
        }
        return true;
    }

    size_t newLength = m_buffer.length();
    size_t newCapacity = m_buffer.capacity();
    unsigned char* newBuffer = m_buffer.extractRawBuffer();

    // NB: extractRawBuffer() only returns null if the Vector is using
    // inline storage and thus a malloc would be needed. In this case,
    // just make a simple copy.
    if (!newBuffer)
        return bytes.append(m_buffer.begin(), m_buffer.end());

    bytes.replaceRawBuffer((uint8_t*)newBuffer, newLength, newCapacity);
    return true;
}

#ifdef JS_JITSPEW
void
js::jit::GenericAssembler::spew(const char* fmt, va_list va)
{
    // Buffer to hold the formatted string. Note that this may contain
    // '%' characters, so do not pass it directly to printf functions.
    char buf[200];

    int i = VsprintfLiteral(buf, fmt, va);
    if (i > -1) {
        if (printer)
            printer->printf("%s\n", buf);
        js::jit::JitSpew(js::jit::JitSpew_Codegen, "%s", buf);
    }
}
#endif
