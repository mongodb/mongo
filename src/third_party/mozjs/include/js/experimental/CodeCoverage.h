/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_experimental_CodeCoverage_h
#define js_experimental_CodeCoverage_h

#include "jstypes.h"     // JS_PUBLIC_API
#include "js/Utility.h"  // JS::UniqueChars

struct JS_PUBLIC_API JSContext;

namespace js {

/**
 * Enable the collection of lcov code coverage metrics.
 * Must be called before a runtime is created and before any calls to
 * GetCodeCoverageSummary.
 */
extern JS_PUBLIC_API void EnableCodeCoverage();

/**
 * Generate lcov trace file content for the current realm, and allocate a new
 * buffer and return the content in it, the size of the newly allocated content
 * within the buffer would be set to the length out-param. The 'All' variant
 * will collect data for all realms in the runtime.
 *
 * In case of out-of-memory, this function returns nullptr. The length
 * out-param is undefined on failure.
 */
extern JS_PUBLIC_API JS::UniqueChars GetCodeCoverageSummary(JSContext* cx,
                                                            size_t* length);
extern JS_PUBLIC_API JS::UniqueChars GetCodeCoverageSummaryAll(JSContext* cx,
                                                               size_t* length);

}  // namespace js

#endif  // js_experimental_CodeCoverage_h
