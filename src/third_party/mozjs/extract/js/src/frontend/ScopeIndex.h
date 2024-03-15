/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef frontend_ScopeIndex_h
#define frontend_ScopeIndex_h

#include <cstdint>  // uint32_t, UINT32_MAX

#include "frontend/TypedIndex.h"  // TypedIndex

namespace js {

class Scope;

class ScopeIndex : public frontend::TypedIndex<Scope> {
  // Delegate constructors;
  using Base = frontend::TypedIndex<Scope>;
  using Base::Base;

  static constexpr uint32_t InvalidIndex = UINT32_MAX;

 public:
  static constexpr ScopeIndex invalid() { return ScopeIndex(InvalidIndex); }
  bool isValid() const { return index != InvalidIndex; }
};

} /* namespace js */

#endif /* frontend_ScopeIndex_h */
