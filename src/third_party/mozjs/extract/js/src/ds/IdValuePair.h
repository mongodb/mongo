/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ds_IdValuePair_h
#define ds_IdValuePair_h

#include "gc/Tracer.h"
#include "js/GCVector.h"
#include "js/Id.h"
#include "js/Value.h"

namespace js {

struct IdValuePair {
  JS::Value value;
  jsid id;

  IdValuePair() : value(JS::UndefinedValue()), id(JSID_VOID) {}
  explicit IdValuePair(jsid idArg) : value(JS::UndefinedValue()), id(idArg) {}
  IdValuePair(jsid idArg, const Value& valueArg) : value(valueArg), id(idArg) {}

  void trace(JSTracer* trc) {
    TraceRoot(trc, &value, "IdValuePair::value");
    TraceRoot(trc, &id, "IdValuePair::id");
  }
};

using IdValueVector = JS::GCVector<IdValuePair, 8>;

} /* namespace js */

#endif /* ds_IdValuePair_h */
