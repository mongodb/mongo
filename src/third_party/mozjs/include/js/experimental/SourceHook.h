/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * The context-wide source hook allows the source of scripts/functions to be
 * discarded, if that source is constant and readily-reloadable if it's needed
 * in the future.
 *
 * Ordinarily, functions and scripts store a copy of their underlying source, to
 * support |Function.prototype.toString| and debuggers.  Some scripts, however,
 * might be constant and retrievable on demand -- perhaps burned into the binary
 * or in a readonly file provided by the embedding.  Why not just ask the
 * embedding for a copy of the source?
 *
 * The context-wide |SourceHook| gives embedders a way to respond to these
 * requests.  The source of scripts/functions compiled with the compile option
 * |JS::CompileOptions::setSourceIsLazy(true)| is eligible to be discarded.
 * (The exact conditions under which source is discarded are unspecified.)  *If*
 * source is discarded, performing an operation that requires source uses the
 * source hook to load the source.
 *
 * The source hook must return the *exact* same source for every call.  (This is
 * why the source hook is unsuitable for use with scripts loaded from the web:
 * in general, their contents can change over time.)  If the source hook doesn't
 * return the exact same source, Very Bad Things may happen.  (For example,
 * previously-valid indexes into the source will no longer be coherent: they
 * might index out of bounds, into the middle of multi-unit code points, &c.)
 *
 * These APIs are experimental because they shouldn't provide a per-*context*
 * mechanism, rather something that's per-compilation.
 */

#ifndef js_experimental_SourceHook_h
#define js_experimental_SourceHook_h

#include "mozilla/UniquePtr.h"  // mozilla::UniquePtr

#include <stddef.h>  // size_t

#include "jstypes.h"  // JS_PUBLIC_API

struct JS_PUBLIC_API JSContext;

namespace js {

/**
 * A class of objects that return source code on demand.
 *
 * When code is compiled with setSourceIsLazy(true), SpiderMonkey doesn't
 * retain the source code (and doesn't do lazy bytecode generation). If we ever
 * need the source code, say, in response to a call to Function.prototype.
 * toSource or Debugger.Source.prototype.text, then we call the 'load' member
 * function of the instance of this class that has hopefully been registered
 * with the runtime, passing the code's URL, and hope that it will be able to
 * find the source.
 */
class SourceHook {
 public:
  virtual ~SourceHook() = default;

  /**
   * Attempt to load the source for |filename|.
   *
   * On success, return true and store an owning pointer to the UTF-8 or UTF-16
   * contents of the file in whichever of |twoByteSource| or |utf8Source| is
   * non-null.  (Exactly one of these will be non-null.)  If the stored pointer
   * is non-null, source was loaded and must be |js_free|'d when it's no longer
   * needed.  If the stored pointer is null, the JS engine will simply act as if
   * source was unavailable, and users like |Function.prototype.toString| will
   * produce fallback results, e.g. "[native code]".
   *
   * On failure, return false.  The contents of whichever of |twoByteSource| or
   * |utf8Source| was initially non-null are unspecified and must not be
   * |js_free|'d.
   */
  virtual bool load(JSContext* cx, const char* filename,
                    char16_t** twoByteSource, char** utf8Source,
                    size_t* length) = 0;
};

/**
 * Have |cx| use |hook| to retrieve lazily-retrieved source code. See the
 * comments for SourceHook. The context takes ownership of the hook, and
 * will delete it when the context itself is deleted, or when a new hook is
 * set.
 */
extern JS_PUBLIC_API void SetSourceHook(JSContext* cx,
                                        mozilla::UniquePtr<SourceHook> hook);

/** Remove |cx|'s source hook, and return it. The caller now owns the hook. */
extern JS_PUBLIC_API mozilla::UniquePtr<SourceHook> ForgetSourceHook(
    JSContext* cx);

}  // namespace js

#endif  // js_experimental_SourceHook_h
