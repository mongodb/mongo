/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* SpiderMonkey API for obtaining JitCode information. */

#ifndef js_JitCodeAPI_h
#define js_JitCodeAPI_h

#include "js/AllocPolicy.h"
#include "js/ColumnNumber.h"  // JS::LimitedColumnNumberOneOrigin
#include "js/Initialization.h"
#include "js/Printf.h"
#include "js/Vector.h"

namespace JS {

enum class JitTier { Baseline, IC, Ion, Other };

class JitOpcodeDictionary {
  typedef js::Vector<UniqueChars, 0, js::SystemAllocPolicy> StringVector;

 public:
  JitOpcodeDictionary();

  StringVector& GetBaselineDictionary() { return baselineDictionary; }
  StringVector& GetIonDictionary() { return ionDictionary; }
  StringVector& GetInlineCacheDictionary() { return icDictionary; }

 private:
  StringVector baselineDictionary;
  StringVector icDictionary;
  StringVector ionDictionary;
};

struct JitCodeSourceInfo {
  UniqueChars filename;
  uint32_t offset = 0;

  // Line number (1-origin).
  uint32_t lineno = 0;
  // Column number in UTF-16 code units.
  JS::LimitedColumnNumberOneOrigin colno;
};

struct JitCodeIRInfo {
  uint32_t offset = 0;
  uint32_t opcode = 0;
  UniqueChars str;
};

typedef js::Vector<JitCodeSourceInfo, 0, js::SystemAllocPolicy>
    SourceInfoVector;
typedef js::Vector<JitCodeIRInfo, 0, js::SystemAllocPolicy> IRInfoVector;

struct JitCodeRecord {
  UniqueChars functionName;
  uint64_t code_addr = 0;
  uint32_t instructionSize = 0;
  JitTier tier = JitTier::Other;

  SourceInfoVector sourceInfo;
  IRInfoVector irInfo;
};

class JitCodeIterator {
  void getDataForIndex(size_t iteratorIndex);

 public:
  JitCodeIterator();
  ~JitCodeIterator();

  void operator++(int) {
    iteratorIndex++;
    getDataForIndex(iteratorIndex);
  }

  explicit operator bool() const { return data != nullptr; }

  SourceInfoVector& sourceData() { return data->sourceInfo; }

  IRInfoVector& irData() { return data->irInfo; }

  const char* functionName() const { return data->functionName.get(); }
  uint64_t code_addr() const { return data->code_addr; }
  uint32_t instructionSize() { return data->instructionSize; }
  JitTier jit_tier() const { return data->tier; }

 private:
  JitCodeRecord* data = nullptr;
  size_t iteratorIndex = 0;
};

}  // namespace JS

#endif /* js_JitCodeAPI_h */
