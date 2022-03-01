/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef util_CompleteFile_h
#define util_CompleteFile_h

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include <stdint.h>  // uint8_t
#include <stdio.h>   // fclose, FILE, stdin

#include "jstypes.h"         // JS_PUBLIC_API
#include "js/AllocPolicy.h"  // js::TempAllocPolicy
#include "js/Vector.h"       // js::Vector

struct JS_PUBLIC_API JSContext;

namespace js {

using FileContents = Vector<uint8_t, 8, TempAllocPolicy>;

extern bool ReadCompleteFile(JSContext* cx, FILE* fp, FileContents& buffer);

class AutoFile {
  FILE* fp_ = nullptr;

 public:
  AutoFile() = default;

  ~AutoFile() {
    if (fp_ && fp_ != stdin) {
      fclose(fp_);
    }
  }

  FILE* fp() const { return fp_; }

  bool open(JSContext* cx, const char* filename);

  bool readAll(JSContext* cx, FileContents& buffer) {
    MOZ_ASSERT(fp_);
    return ReadCompleteFile(cx, fp_, buffer);
  }
};

}  // namespace js

#endif /* util_CompleteFile_h */
