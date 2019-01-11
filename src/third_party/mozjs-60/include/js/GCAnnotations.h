/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_GCAnnotations_h
#define js_GCAnnotations_h

// Set of annotations for the rooting hazard analysis, used to categorize types
// and functions.
#ifdef XGILL_PLUGIN

// Mark a type as being a GC thing (eg js::gc::Cell has this annotation).
# define JS_HAZ_GC_THING __attribute__((tag("GC Thing")))

// Mark a type as holding a pointer to a GC thing (eg JS::Value has this
// annotation.)
# define JS_HAZ_GC_POINTER __attribute__((tag("GC Pointer")))

// Mark a type as a rooted pointer, suitable for use on the stack (eg all
// Rooted<T> instantiations should have this.)
# define JS_HAZ_ROOTED __attribute__((tag("Rooted Pointer")))

// Mark a type as something that should not be held live across a GC, but which
// is not itself a GC pointer.
# define JS_HAZ_GC_INVALIDATED __attribute__((tag("Invalidated by GC")))

// Mark a type that would otherwise be considered a GC Pointer (eg because it
// contains a JS::Value field) as a non-GC pointer. It is handled almost the
// same in the analysis as a rooted pointer, except it will not be reported as
// an unnecessary root if used across a GC call. This should rarely be used,
// but makes sense for something like ErrorResult, which only contains a GC
// pointer when it holds an exception (and it does its own rooting,
// conditionally.)
# define JS_HAZ_NON_GC_POINTER __attribute__((tag("Suppressed GC Pointer")))

// Mark a function as something that runs a garbage collection, potentially
// invalidating GC pointers.
# define JS_HAZ_GC_CALL __attribute__((tag("GC Call")))

// Mark an RAII class as suppressing GC within its scope.
# define JS_HAZ_GC_SUPPRESSED __attribute__((tag("Suppress GC")))

#else

# define JS_HAZ_GC_THING
# define JS_HAZ_GC_POINTER
# define JS_HAZ_ROOTED
# define JS_HAZ_GC_INVALIDATED
# define JS_HAZ_NON_GC_POINTER
# define JS_HAZ_GC_CALL
# define JS_HAZ_GC_SUPPRESSED

#endif

#endif /* js_GCAnnotations_h */
