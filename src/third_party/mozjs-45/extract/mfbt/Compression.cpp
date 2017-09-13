/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/Compression.h"
#include "mozilla/CheckedInt.h"

// Without including <string>, MSVC 2015 complains about e.g. the impossibility
// to convert `const void* const` to `void*` when calling memchr from
// corecrt_memory.h.
#include <string>

using namespace mozilla::Compression;

namespace {

#include "lz4.c"

}/* anonymous namespace */

/* Our wrappers */

size_t
LZ4::compress(const char* aSource, size_t aInputSize, char* aDest)
{
  CheckedInt<int> inputSizeChecked = aInputSize;
  MOZ_ASSERT(inputSizeChecked.isValid());
  return LZ4_compress(aSource, aDest, inputSizeChecked.value());
}

size_t
LZ4::compressLimitedOutput(const char* aSource, size_t aInputSize, char* aDest,
                           size_t aMaxOutputSize)
{
  CheckedInt<int> inputSizeChecked = aInputSize;
  MOZ_ASSERT(inputSizeChecked.isValid());
  CheckedInt<int> maxOutputSizeChecked = aMaxOutputSize;
  MOZ_ASSERT(maxOutputSizeChecked.isValid());
  return LZ4_compress_limitedOutput(aSource, aDest, inputSizeChecked.value(),
                                    maxOutputSizeChecked.value());
}

bool
LZ4::decompress(const char* aSource, char* aDest, size_t aOutputSize)
{
  CheckedInt<int> outputSizeChecked = aOutputSize;
  MOZ_ASSERT(outputSizeChecked.isValid());
  int ret = LZ4_decompress_fast(aSource, aDest, outputSizeChecked.value());
  return ret >= 0;
}

bool
LZ4::decompress(const char* aSource, size_t aInputSize, char* aDest,
                size_t aMaxOutputSize, size_t* aOutputSize)
{
  CheckedInt<int> maxOutputSizeChecked = aMaxOutputSize;
  MOZ_ASSERT(maxOutputSizeChecked.isValid());
  CheckedInt<int> inputSizeChecked = aInputSize;
  MOZ_ASSERT(inputSizeChecked.isValid());

  int ret = LZ4_decompress_safe(aSource, aDest, inputSizeChecked.value(),
                                maxOutputSizeChecked.value());
  if (ret >= 0) {
    *aOutputSize = ret;
    return true;
  }

  *aOutputSize = 0;
  return false;
}

