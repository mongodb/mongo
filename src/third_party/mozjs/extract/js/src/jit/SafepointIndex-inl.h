/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_SafepointIndex_inl_h
#define jit_SafepointIndex_inl_h

#include "jit/SafepointIndex.h"

#include "jit/LIR.h"

namespace js::jit {

inline SafepointIndex::SafepointIndex(const CodegenSafepointIndex& csi)
    : displacement_(csi.displacement()),
      safepointOffset_(csi.safepoint()->offset()) {}

}  // namespace js::jit

#endif /* jit_SafepointIndex_inl_h */
