/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2021 Mozilla Foundation
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

#ifndef wasm_exception_h
#define wasm_exception_h

namespace js {
namespace wasm {

static const uint32_t CatchAllIndex = UINT32_MAX;
static_assert(CatchAllIndex > MaxTags);

struct TryTableCatch {
  TryTableCatch()
      : tagIndex(CatchAllIndex), labelRelativeDepth(0), captureExnRef(false) {}

  // The tag index for this catch, or CatchAllIndex for a catch_all.
  uint32_t tagIndex;
  // The relative depth of where to branch to when catching an exception.
  uint32_t labelRelativeDepth;
  // Whether the exnref that is caught should be captured and passed to the
  // branch target.
  bool captureExnRef;
  // The params that the target branch at `labelRelativeDepth` expects. This
  // includes any exnref that should or should not be captured.
  ValTypeVector labelType;
};
using TryTableCatchVector = Vector<TryTableCatch, 1, SystemAllocPolicy>;

}  // namespace wasm
}  // namespace js

#endif  // wasm_exception_h
