/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_friend_PerformanceHint_h
#define js_friend_PerformanceHint_h

namespace js {
namespace gc {

// API to let the DOM tell us whether we're currently in pageload, so we can
// change the GC triggers to discourage collection of the atoms zone.
//
// This is a temporary measure until parsing is changed to not allocate GC
// things off the main thread.

enum class PerformanceHint { Normal, InPageLoad };

extern JS_PUBLIC_API void SetPerformanceHint(JSContext* cx,
                                             PerformanceHint hint);

} /* namespace gc */
} /* namespace js */

#endif  // js_friend_PerformanceHint_h
