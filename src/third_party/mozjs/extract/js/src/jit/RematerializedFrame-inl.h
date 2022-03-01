/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_RematerializedFrame_inl_h
#define jit_RematerializedFrame_inl_h

#include "jit/RematerializedFrame.h"

#include "mozilla/Assertions.h"  // MOZ_ASSERT

#include "vm/JSScript.h"  // JSScript

#include "vm/JSScript-inl.h"  // JSScript::isDebuggee

inline void js::jit::RematerializedFrame::unsetIsDebuggee() {
  MOZ_ASSERT(!script()->isDebuggee());
  isDebuggee_ = false;
}

#endif  // jit_RematerializedFrame_inl_h
