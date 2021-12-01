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

#ifndef VIXL_CPU_A64_H
#define VIXL_CPU_A64_H

#include "jit/arm64/vixl/Globals-vixl.h"
#include "jit/arm64/vixl/Instructions-vixl.h"

namespace vixl {

class CPU {
 public:
  // Initialise CPU support.
  static void SetUp();

  // Ensures the data at a given address and with a given size is the same for
  // the I and D caches. I and D caches are not automatically coherent on ARM
  // so this operation is required before any dynamically generated code can
  // safely run.
  static void EnsureIAndDCacheCoherency(void *address, size_t length);

  // Handle tagged pointers.
  template <typename T>
  static T SetPointerTag(T pointer, uint64_t tag) {
    VIXL_ASSERT(is_uintn(kAddressTagWidth, tag));

    // Use C-style casts to get static_cast behaviour for integral types (T),
    // and reinterpret_cast behaviour for other types.

    uint64_t raw = (uint64_t)pointer;
    VIXL_STATIC_ASSERT(sizeof(pointer) == sizeof(raw));

    raw = (raw & ~kAddressTagMask) | (tag << kAddressTagOffset);
    return (T)raw;
  }

  template <typename T>
  static uint64_t GetPointerTag(T pointer) {
    // Use C-style casts to get static_cast behaviour for integral types (T),
    // and reinterpret_cast behaviour for other types.

    uint64_t raw = (uint64_t)pointer;
    VIXL_STATIC_ASSERT(sizeof(pointer) == sizeof(raw));

    return (raw & kAddressTagMask) >> kAddressTagOffset;
  }

 private:
  // Return the content of the cache type register.
  static uint32_t GetCacheType();

  // I and D cache line size in bytes.
  static unsigned icache_line_size_;
  static unsigned dcache_line_size_;
};

}  // namespace vixl

#endif  // VIXL_CPU_A64_H
