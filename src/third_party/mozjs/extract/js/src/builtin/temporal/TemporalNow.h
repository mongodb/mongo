/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_temporal_TemporalNow_h
#define builtin_temporal_TemporalNow_h

#include "vm/NativeObject.h"

struct JSClass;

namespace js {
struct ClassSpec;
}

namespace js::temporal {

class TemporalNowObject : public NativeObject {
 public:
  static const JSClass class_;

 private:
  static const ClassSpec classSpec_;
};

} /* namespace js::temporal */

#endif /* builtin_temporal_TemporalNow_h */
