
// Copyright 2022 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef jit_riscv64_constant_util_riscv64__h_
#define jit_riscv64_constant_util_riscv64__h_
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
namespace js {
namespace jit {
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
  EmbeddedVector() : V8Vector<T>(buffer_, kSize) {}

  explicit EmbeddedVector(T initial_value) : V8Vector<T>(buffer_, kSize) {
    for (int i = 0; i < kSize; ++i) {
      buffer_[i] = initial_value;
    }
  }

  // When copying, make underlying Vector to reference our buffer.
  EmbeddedVector(const EmbeddedVector& rhs) : V8Vector<T>(rhs) {
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

// Helper function for printing to a Vector.
static inline int MOZ_FORMAT_PRINTF(2, 3)
    SNPrintF(V8Vector<char> str, const char* format, ...) {
  va_list args;
  va_start(args, format);
  int result = vsnprintf(str.start(), str.length(), format, args);
  va_end(args);
  return result;
}
}  // namespace jit
}  // namespace js
#endif
