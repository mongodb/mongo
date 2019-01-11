/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2016 Mozilla Foundation
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

#ifndef wasm_text_utils
#define wasm_text_utils

#include "NamespaceImports.h"

#include "util/StringBuffer.h"

namespace js {
namespace wasm {

template<size_t base>
MOZ_MUST_USE bool
RenderInBase(StringBuffer& sb, uint64_t num);

template<class T>
MOZ_MUST_USE bool
RenderNaN(StringBuffer& sb, T num);

// Helper class, StringBuffer wrapper, to track the position (line and column)
// within the generated source.

class WasmPrintBuffer
{
    StringBuffer& stringBuffer_;
    uint32_t lineno_;
    uint32_t column_;

  public:
    explicit WasmPrintBuffer(StringBuffer& stringBuffer)
      : stringBuffer_(stringBuffer),
        lineno_(1),
        column_(1)
    {}
    inline char processChar(char ch) {
        if (ch == '\n') {
            lineno_++; column_ = 1;
        } else
            column_++;
        return ch;
    }
    inline char16_t processChar(char16_t ch) {
        if (ch == '\n') {
            lineno_++; column_ = 1;
        } else
            column_++;
        return ch;
    }
    bool append(const char ch) {
        return stringBuffer_.append(processChar(ch));
    }
    bool append(const char16_t ch) {
        return stringBuffer_.append(processChar(ch));
    }
    bool append(const char* str, size_t length) {
        for (size_t i = 0; i < length; i++)
            processChar(str[i]);
        return stringBuffer_.append(str, length);
    }
    bool append(const char16_t* begin, const char16_t* end) {
        for (const char16_t* p = begin; p != end; p++)
            processChar(*p);
        return stringBuffer_.append(begin, end);
    }
    bool append(const char16_t* str, size_t length) {
        return append(str, str + length);
    }
    template <size_t ArrayLength>
    bool append(const char (&array)[ArrayLength]) {
        static_assert(ArrayLength > 0, "null-terminated");
        MOZ_ASSERT(array[ArrayLength - 1] == '\0');
        return append(array, ArrayLength - 1);
    }
    char16_t getChar(size_t index) {
        return stringBuffer_.getChar(index);
    }
    size_t length() {
        return stringBuffer_.length();
    }
    StringBuffer& stringBuffer() { return stringBuffer_; }
    uint32_t lineno() { return lineno_; }
    uint32_t column() { return column_; }
};

}  // namespace wasm
}  // namespace js

#endif // namespace wasm_text_utils
