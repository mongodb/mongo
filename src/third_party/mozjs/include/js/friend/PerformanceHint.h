/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_friend_PerformanceHint_h
#define js_friend_PerformanceHint_h

#include "jstypes.h"       // JS_PUBLIC_API
#include "js/TypeDecls.h"  // JSContext

namespace js {
namespace gc {

// API to let the DOM tell us whether we're currently in pageload.
//
// This currently affects nursery sizing; we tolerate large nursery sizes (and
// hence longer minor GC pauses) during pageload so as not to limit performance.

enum class PerformanceHint { Normal, InPageLoad };

extern JS_PUBLIC_API void SetPerformanceHint(JSContext* cx,
                                             PerformanceHint hint);

} /* namespace gc */
} /* namespace js */

#endif  // js_friend_PerformanceHint_h
