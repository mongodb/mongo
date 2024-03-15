/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_GCAnnotations_h
#define js_GCAnnotations_h

// Set of annotations for the rooting hazard analysis, used to categorize types
// and functions.
#ifdef XGILL_PLUGIN

#  define JS_EXPECT_HAZARDS __attribute__((annotate("Expect Hazards")))

// Mark a type as being a GC thing (eg js::gc::Cell has this annotation).
#  define JS_HAZ_GC_THING __attribute__((annotate("GC Thing")))

// Mark a type as holding a pointer to a GC thing (eg JS::Value has this
// annotation.) "Inherited" by templatized types with
// MOZ_INHERIT_TYPE_ANNOTATIONS_FROM_TEMPLATE_ARGS.
#  define JS_HAZ_GC_POINTER __attribute__((annotate("GC Pointer")))

// Same as JS_HAZ_GC_POINTER, except additionally treat pointers to these
// as GC pointers themselves in order to check references to them, since
// the analysis cannot distinguish between pointers and references.
#  define JS_HAZ_GC_REF __attribute__((annotate("GC Pointer or Reference")))

// Mark a type as a rooted pointer, suitable for use on the stack (eg all
// Rooted<T> instantiations should have this.) "Inherited" by templatized types
// with MOZ_INHERIT_TYPE_ANNOTATIONS_FROM_TEMPLATE_ARGS.
#  define JS_HAZ_ROOTED __attribute__((annotate("Rooted Pointer")))

// Mark a type as something that should not be held live across a GC, but which
// is not itself a GC pointer. Note that this property is *not* inherited by
// templatized types with MOZ_INHERIT_TYPE_ANNOTATIONS_FROM_TEMPLATE_ARGS.
#  define JS_HAZ_GC_INVALIDATED __attribute__((annotate("Invalidated by GC")))

// Mark a class as a base class of rooted types, eg CustomAutoRooter. All
// descendants of this class will be considered rooted, though classes that
// merely contain these as a field member will not be. "Inherited" by
// templatized types with MOZ_INHERIT_TYPE_ANNOTATIONS_FROM_TEMPLATE_ARGS
#  define JS_HAZ_ROOTED_BASE __attribute__((annotate("Rooted Base")))

// Mark a type that would otherwise be considered a GC Pointer (eg because it
// contains a JS::Value field) as a non-GC pointer. It is handled almost the
// same in the analysis as a rooted pointer, except it will not be reported as
// an unnecessary root if used across a GC call. This should rarely be used,
// but makes sense for something like ErrorResult, which only contains a GC
// pointer when it holds an exception (and it does its own rooting,
// conditionally.)
#  define JS_HAZ_NON_GC_POINTER \
    __attribute__((annotate("Suppressed GC Pointer")))

// Mark a function as something that runs a garbage collection, potentially
// invalidating GC pointers.
#  define JS_HAZ_GC_CALL __attribute__((annotate("GC Call")))

// Mark an RAII class as suppressing GC within its scope.
#  define JS_HAZ_GC_SUPPRESSED __attribute__((annotate("Suppress GC")))

// Mark a function as one that can run script if called.  This obviously
// subsumes JS_HAZ_GC_CALL, since anything that can run script can GC.`
#  define JS_HAZ_CAN_RUN_SCRIPT __attribute__((annotate("Can run script")))

// Mark a function as able to call JSNatives. Otherwise, JSNatives don't show
// up in the callgraph. This doesn't matter for the can-GC analysis, but it is
// very nice for other uses of the callgraph.
#  define JS_HAZ_JSNATIVE_CALLER __attribute__((annotate("Calls JSNatives")))

// Mark a variable as being "GC safe", i.e., it does not contain any
// invalidatable pointers at the current point in the code. A typical
// example might be a collection containing GC pointers, which at the
// present time is empty. This property is only temporary; the next use
// of the variable will invalidate it (on the assumption that a GC pointer
// might be added to it.) Try to use this as early as possible, probably
// immediately after construction, so that if future mutations through
// the variable are added, they won't be covered by the annotation.
#  define JS_HAZ_VALUE_IS_GC_SAFE(var) JS::detail::MarkVariableAsGCSafe(var)

#else

#  define JS_EXPECT_HAZARDS
#  define JS_HAZ_GC_THING
#  define JS_HAZ_GC_POINTER
#  define JS_HAZ_GC_REF
#  define JS_HAZ_ROOTED
#  define JS_HAZ_GC_INVALIDATED
#  define JS_HAZ_ROOTED_BASE
#  define JS_HAZ_NON_GC_POINTER
#  define JS_HAZ_GC_CALL
#  define JS_HAZ_GC_SUPPRESSED
#  define JS_HAZ_CAN_RUN_SCRIPT
#  define JS_HAZ_JSNATIVE_CALLER
#  define JS_HAZ_VALUE_IS_GC_SAFE(var)

#endif

#ifdef XGILL_PLUGIN

// Implemented by passing variable to a dummy function so that it shows up
// in the control flow graph.
namespace JS {
namespace detail {

template <typename T>
static inline void MarkVariableAsGCSafe(T& var) {
  asm("");
}

}  // namespace detail
}  // namespace JS

#endif

#endif /* js_GCAnnotations_h */
