/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_RegExpStubConstants_h
#define jit_RegExpStubConstants_h

#include <stddef.h>
#include <stdint.h>

#include "irregexp/RegExpTypes.h"
#include "vm/MatchPairs.h"

namespace js {
namespace jit {

static constexpr size_t InputOutputDataSize = sizeof(irregexp::InputOutputData);

// Amount of space to reserve on the stack when executing RegExps inline.
static constexpr size_t RegExpReservedStack =
    InputOutputDataSize + sizeof(MatchPairs) +
    RegExpObject::MaxPairCount * sizeof(MatchPair);

// RegExpExecTest return value to indicate failure.
static constexpr int32_t RegExpExecTestResultFailed = -1;

// RegExpSearcher return values to indicate not-found or failure.
static constexpr int32_t RegExpSearcherResultNotFound = -1;
static constexpr int32_t RegExpSearcherResultFailed = -2;

}  // namespace jit
}  // namespace js

#endif /* jit_RegExpStubConstants_h */
