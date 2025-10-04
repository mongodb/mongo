// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TCMALLOC_INTERNAL_MOCK_SPAN_H_
#define TCMALLOC_INTERNAL_MOCK_SPAN_H_

#include "tcmalloc/internal/linked_list.h"

namespace tcmalloc {
namespace tcmalloc_internal {

class MockSpan;
typedef TList<MockSpan> MockSpanList;

class MockSpan : public MockSpanList::Elem {
 public:
  MockSpan() {}

  static MockSpan* New(int idx = 0) {
    MockSpan* ret = new MockSpan();
    ret->index_ = idx;
    return ret;
  }

  int index_;
};

}  // namespace tcmalloc_internal
}  // namespace tcmalloc

#endif  // TCMALLOC_INTERNAL_MOCK_SPAN_H_
