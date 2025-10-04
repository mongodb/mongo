/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef gc_ClearEdgesTracer_h
#define gc_ClearEdgesTracer_h

#include "js/TracingAPI.h"

namespace js {
namespace gc {

struct ClearEdgesTracer final : public GenericTracerImpl<ClearEdgesTracer> {
  explicit ClearEdgesTracer(JSRuntime* rt);

 private:
  template <typename T>
  void onEdge(T** thingp, const char* name);
  friend class GenericTracerImpl<ClearEdgesTracer>;
};

}  // namespace gc
}  // namespace js

#endif  // gc_ClearEdgesTracer_h
