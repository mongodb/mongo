/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 */
// Copyright 2007-2008 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef jit_arm_disasm_Disasm_arm_h
#define jit_arm_disasm_Disasm_arm_h

#ifdef JS_DISASM_ARM

#include "mozilla/Assertions.h"
#include "mozilla/Types.h"

#include <stdio.h>

namespace js {
namespace jit {
namespace disasm {

typedef unsigned char byte;

// A reasonable (ie, safe) buffer size for the disassembly of a single instruction.
const int ReasonableBufferSize = 256;

// Vector as used by the original code to allow for minimal modification.
// Functions exactly like a character array with helper methods.
template <typename T>
class V8Vector {
  public:
    V8Vector() : start_(nullptr), length_(0) {}
    V8Vector(T* data, int length) : start_(data), length_(length) {
        MOZ_ASSERT(length == 0 || (length > 0 && data != nullptr));
    }

    // Returns the length of the vector.
    int length() const { return length_; }

    // Returns the pointer to the start of the data in the vector.
    T* start() const { return start_; }

    // Access individual vector elements - checks bounds in debug mode.
    T& operator[](int index) const {
        MOZ_ASSERT(0 <= index && index < length_);
        return start_[index];
    }

    inline V8Vector<T> operator+(int offset) {
        MOZ_ASSERT(offset < length_);
        return V8Vector<T>(start_ + offset, length_ - offset);
    }

  private:
    T* start_;
    int length_;
};


template <typename T, int kSize>
class EmbeddedVector : public V8Vector<T> {
  public:
    EmbeddedVector() : V8Vector<T>(buffer_, kSize) { }

    explicit EmbeddedVector(T initial_value) : V8Vector<T>(buffer_, kSize) {
        for (int i = 0; i < kSize; ++i) {
            buffer_[i] = initial_value;
        }
    }

    // When copying, make underlying Vector to reference our buffer.
    EmbeddedVector(const EmbeddedVector& rhs)
        : V8Vector<T>(rhs) {
        MemCopy(buffer_, rhs.buffer_, sizeof(T) * kSize);
        this->set_start(buffer_);
    }

    EmbeddedVector& operator=(const EmbeddedVector& rhs) {
        if (this == &rhs) return *this;
        V8Vector<T>::operator=(rhs);
        MemCopy(buffer_, rhs.buffer_, sizeof(T) * kSize);
        this->set_start(buffer_);
        return *this;
    }

  private:
    T buffer_[kSize];
};


// Interface and default implementation for converting addresses and
// register-numbers to text.  The default implementation is machine
// specific.
class NameConverter {
  public:
    virtual ~NameConverter() {}
    virtual const char* NameOfCPURegister(int reg) const;
    virtual const char* NameOfByteCPURegister(int reg) const;
    virtual const char* NameOfXMMRegister(int reg) const;
    virtual const char* NameOfAddress(byte* addr) const;
    virtual const char* NameOfConstant(byte* addr) const;
    virtual const char* NameInCode(byte* addr) const;

  protected:
    EmbeddedVector<char, 128> tmp_buffer_;
};


// A generic Disassembler interface
class Disassembler {
  public:
    // Caller deallocates converter.
    explicit Disassembler(const NameConverter& converter);

    virtual ~Disassembler();

    // Writes one disassembled instruction into 'buffer' (0-terminated).
    // Returns the length of the disassembled machine instruction in bytes.
    int InstructionDecode(V8Vector<char> buffer, uint8_t* instruction);

    // Returns -1 if instruction does not mark the beginning of a constant pool,
    // or the number of entries in the constant pool beginning here.
    int ConstantPoolSizeAt(byte* instruction);

    // Write disassembly into specified file 'f' using specified NameConverter
    // (see constructor).
    static void Disassemble(FILE* f, uint8_t* begin, uint8_t* end);
  private:
    const NameConverter& converter_;

    // Disallow implicit constructors.
    Disassembler() = delete;
    Disassembler(const Disassembler&) = delete;
    void operator=(const Disassembler&) = delete;
};

}  // namespace disasm
}  // namespace jit
}  // namespace js

#endif // JS_DISASM_ARM

#endif  // jit_arm_disasm_Disasm_arm_h
