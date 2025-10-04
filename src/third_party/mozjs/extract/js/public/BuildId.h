/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/**
 * Embedding-provided build ID information, used by SpiderMonkey to tag cached
 * compilation data so that cached data can be reused when possible, or
 * discarded and regenerated if necessary.
 */

#ifndef js_BuildId_h
#define js_BuildId_h

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/Vector.h"  // js::Vector

namespace js {

class SystemAllocPolicy;

}  // namespace js

namespace JS {

/** Vector of characters used for holding build ids. */
using BuildIdCharVector = js::Vector<char, 0, js::SystemAllocPolicy>;

/**
 * Return the buildId (represented as a sequence of characters) associated with
 * the currently-executing build. If the JS engine is embedded such that a
 * single cache entry can be observed by different compiled versions of the JS
 * engine, it is critical that the buildId shall change for each new build of
 * the JS engine.
 */
using BuildIdOp = bool (*)(BuildIdCharVector* buildId);

/**
 * Embedder hook to set the buildId-generating function.
 */
extern JS_PUBLIC_API void SetProcessBuildIdOp(BuildIdOp buildIdOp);

/**
 * Some cached data is, in addition to being build-specific, CPU-specific: the
 * cached data depends on CPU features like a particular level of SSE support.
 *
 * This function produces a buildId that includes:
 *
 *   * the buildId defined by the embedder-provided BuildIdOp set by
 *     JS::SetProcessBuildIdOp, and
 *   * CPU feature information for the current CPU.
 *
 * Embedders may use this function to tag cached data whose validity depends
 * on having consistent buildId *and* on the CPU supporting features identical
 * to those in play when the cached data was computed.
 */
[[nodiscard]] extern JS_PUBLIC_API bool GetOptimizedEncodingBuildId(
    BuildIdCharVector* buildId);

/**
 * Script bytecode is dependent on the buildId and a few other things.
 *
 * This function produces a buildId that includes:
 *
 *   * The buildId defined by the embedder-provided BuildIdOp set by
 *     JS::SetProcessBuildIdOp.
 *   * Additional bytes describing things like endianness, pointer size and
 *     other state XDR buffers depend on.
 *
 * Note: this value may depend on runtime preferences so isn't guaranteed to be
 * stable across restarts.
 *
 * Embedders should use this function to tag transcoded bytecode.
 * See Transcoding.h.
 */
[[nodiscard]] extern JS_PUBLIC_API bool GetScriptTranscodingBuildId(
    BuildIdCharVector* buildId);

}  // namespace JS

#endif /* js_BuildId_h */
