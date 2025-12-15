/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// Helper header to declare all the supported malloc functions.
// MALLOC_DECL arguments are:
//   - function name
//   - return type
//   - argument types

#ifndef malloc_decls_h
#  define malloc_decls_h

#  include "mozjemalloc_types.h"

#  define MALLOC_FUNCS_MALLOC_BASE 1
#  define MALLOC_FUNCS_MALLOC_EXTRA 2
#  define MALLOC_FUNCS_MALLOC \
    (MALLOC_FUNCS_MALLOC_BASE | MALLOC_FUNCS_MALLOC_EXTRA)
#  define MALLOC_FUNCS_JEMALLOC 4
#  define MALLOC_FUNCS_ARENA_BASE 8
#  define MALLOC_FUNCS_ARENA_ALLOC 16
#  define MALLOC_FUNCS_ARENA \
    (MALLOC_FUNCS_ARENA_BASE | MALLOC_FUNCS_ARENA_ALLOC)
#  define MALLOC_FUNCS_ALL \
    (MALLOC_FUNCS_MALLOC | MALLOC_FUNCS_JEMALLOC | MALLOC_FUNCS_ARENA)

// Some malloc operations require extra includes.  Before using this header with
// MALLOC_FUNCS unset or containing MALLOC_FUNCS_JEMALLOC you must include
// this header once, outside a struct/class definition and without MALLOC_DECL
// set and it will include headers it may later need.
#  if !defined(MALLOC_DECL) && defined(__cplusplus)
#    include <functional>
#    include "mozilla/Maybe.h"
#  endif

#endif  // malloc_decls_h

#ifndef MALLOC_FUNCS
#  define MALLOC_FUNCS MALLOC_FUNCS_ALL
#endif

#ifdef MALLOC_DECL
// NOTHROW_MALLOC_DECL is intended for functions where the standard library
// declares the functions in question as `throw()`.  Not all platforms
// consistent declare certain functions as `throw()`, though.

// Bionic and OS X don't seem to care about `throw()`ness.
#  if defined(ANDROID) || defined(XP_DARWIN)
#    undef NOTHROW_MALLOC_DECL
#    define NOTHROW_MALLOC_DECL MALLOC_DECL
// Some places don't care about the distinction.
#  elif !defined(NOTHROW_MALLOC_DECL)
#    define NOTHROW_MALLOC_DECL MALLOC_DECL
#  endif

#  if MALLOC_FUNCS & MALLOC_FUNCS_MALLOC_BASE
MALLOC_DECL(malloc, void*, size_t)
MALLOC_DECL(calloc, void*, size_t, size_t)
MALLOC_DECL(realloc, void*, void*, size_t)
NOTHROW_MALLOC_DECL(free, void, void*)
NOTHROW_MALLOC_DECL(memalign, void*, size_t, size_t)
#  endif
#  if MALLOC_FUNCS & MALLOC_FUNCS_MALLOC_EXTRA
NOTHROW_MALLOC_DECL(posix_memalign, int, void**, size_t, size_t)
NOTHROW_MALLOC_DECL(aligned_alloc, void*, size_t, size_t)
NOTHROW_MALLOC_DECL(valloc, void*, size_t)
NOTHROW_MALLOC_DECL(malloc_usable_size, size_t, usable_ptr_t)
MALLOC_DECL(malloc_good_size, size_t, size_t)
#  endif

#  if MALLOC_FUNCS & MALLOC_FUNCS_JEMALLOC
// The 2nd argument points to an optional array exactly
// jemalloc_stats_num_bins() long to be filled in (if non-null).
// This must only be called on the main thread.
MALLOC_DECL(jemalloc_stats_internal, void, jemalloc_stats_t*,
            jemalloc_bin_stats_t*)

// Return the size of the jemalloc_bin_stats_t array.
MALLOC_DECL(jemalloc_stats_num_bins, size_t)

// Return some of the information that jemalloc_stats returns but works
// off-main-thread and is faster.
MALLOC_DECL(jemalloc_stats_lite, void, jemalloc_stats_lite_t*)

// Tell jemalloc this is the main thread. jemalloc will use this to validate
// that main thread only arenas are only used on the main thread.
MALLOC_DECL(jemalloc_set_main_thread, void)

// On some operating systems (Mac), we use madvise(MADV_FREE) to hand pages
// back to the operating system.  On Mac, the operating system doesn't take
// this memory back immediately; instead, the OS takes it back only when the
// machine is running out of physical memory.
//
// This is great from the standpoint of efficiency, but it makes measuring our
// actual RSS difficult, because pages which we've MADV_FREE'd shouldn't count
// against our RSS.
//
// This function explicitly purges any MADV_FREE'd pages from physical memory,
// causing our reported RSS match the amount of memory we're actually using.
//
// Note that this call is expensive in two ways.  First, it may be slow to
// execute, because it may make a number of slow syscalls to free memory.  This
// function holds the big jemalloc locks, so basically all threads are blocked
// while this function runs.
//
// This function is also expensive in that the next time we go to access a page
// which we've just explicitly decommitted, the operating system has to attach
// to it a physical page!  If we hadn't run this function, the OS would have
// less work to do.
//
// If MALLOC_DOUBLE_PURGE is not defined, this function does nothing.
//
// It may only be used from the main thread.
MALLOC_DECL(jemalloc_purge_freed_pages, void)

// Free all unused dirty pages in all arenas. Calling this function will slow
// down subsequent allocations so it is recommended to use it only when
// memory needs to be reclaimed at all costs (see bug 805855). This function
// provides functionality similar to mallctl("arenas.purge") in jemalloc 3.
// Note that if called on a different thread than the main thread, only arenas
// that are not created with ARENA_FLAG_THREAD_MAIN_THREAD_ONLY will be purged.
MALLOC_DECL(jemalloc_free_dirty_pages, void)

// Set the default modifier for mMaxDirty. The value is the number of shifts
// applied to the value. Positive value is handled as <<, negative >>.
// Arenas may override the default modifier.
MALLOC_DECL(moz_set_max_dirty_page_modifier, void, int32_t)

// Enable or disable deferred purging. Returns the former state.
// If enabled, jemalloc will not purge anything until either
// jemalloc_free_[excess]_dirty_pages or moz_may_purge_now are called
// explicitly. Disabling it may cause an immediate synchronous purge of all
// arenas.
// Must be called only on the main thread.
// Parameters:
// bool:            enable/disable
MALLOC_DECL(moz_enable_deferred_purge, bool, bool)

// Perform some purging.
//
// Returns a purge_result_t with the following meaning:
// Done:       Purge has completed for all arenas.
// NeedsMore:  There may be an arena that needs to be purged now.  The caller
//             may call moz_may_purge_one_now again.
// WantsLater: There is at least one arena that might want a purge later,
//             according to aReuseGraceMS passed.  But none requesting purge
//             now.
//
// Parameters:
// aPeekOnly:     If true, it won't process any purge but just return if some is
//                needed now or wanted later.
// aReuseGraceMS: The time to wait after a significant re-use happened before
//                purging memory in an arena.
// aKeepGoing:    Used to determine if it should continue processing purge
//                requests and may be used to implement a work budget.  It will
//                exit if there's no more requests, if it finishes processing an
//                arena or if this parameter returns false.
//
// The cost of calling this when there is no pending purge is: a mutex
// lock/unlock and iterating the list of purges. The mutex is never held during
// expensive operations.
#    ifdef __cplusplus
MALLOC_DECL(moz_may_purge_now, purge_result_t, bool, uint32_t,
            const mozilla::Maybe<std::function<bool()>>&)
#    endif

// Free dirty pages until the max dirty pages threshold is satisfied. Useful
// after lowering the max dirty pages threshold to get RSS back to normal.
// This behaves just like a synchronous purge on all arenas.
// Note that if called on a different thread than the main thread, only arenas
// that are not created with ARENA_FLAG_THREAD_MAIN_THREAD_ONLY will be purged.
MALLOC_DECL(jemalloc_free_excess_dirty_pages, void)

// Change the value of opt_randomize_small to control small allocation
// randomization and maybe perform a reinitialization of the arena's PRNG.
MALLOC_DECL(jemalloc_reset_small_alloc_randomization, void, bool)

// Opt in or out of a thread local arena (bool argument is whether to opt-in
// (true) or out (false)).
MALLOC_DECL(jemalloc_thread_local_arena, void, bool)

// Provide information about any allocation enclosing the given address.
MALLOC_DECL(jemalloc_ptr_info, void, const void*, jemalloc_ptr_info_t*)
#  endif

#  if MALLOC_FUNCS & MALLOC_FUNCS_ARENA_BASE
// Creates a separate arena, and returns its id, valid to use with moz_arena_*
// functions. A helper is provided in mozmemory.h that doesn't take any
// arena_params_t: moz_create_arena.
MALLOC_DECL(moz_create_arena_with_params, arena_id_t, arena_params_t*)

// Dispose of the given arena. Subsequent uses of the arena will crash.
// Passing an invalid id (inexistent or already disposed) to this function
// will crash. The arena must be empty prior to calling this function.
MALLOC_DECL(moz_dispose_arena, void, arena_id_t)
#  endif

#  if MALLOC_FUNCS & MALLOC_FUNCS_ARENA_ALLOC
// Same as the functions without the moz_arena_ prefix, but using arenas
// created with moz_create_arena.
// The contract, even if not enforced at runtime in some configurations,
// is that moz_arena_realloc and moz_arena_free will crash if the given
// arena doesn't own the given pointer. All functions will crash if the
// arena id is invalid.
// Although discouraged, plain realloc and free can still be used on
// pointers allocated with these functions. Realloc will properly keep
// new pointers in the same arena as the original.
MALLOC_DECL(moz_arena_malloc, void*, arena_id_t, size_t)
MALLOC_DECL(moz_arena_calloc, void*, arena_id_t, size_t, size_t)
MALLOC_DECL(moz_arena_realloc, void*, arena_id_t, void*, size_t)
MALLOC_DECL(moz_arena_free, void, arena_id_t, void*)
MALLOC_DECL(moz_arena_memalign, void*, arena_id_t, size_t, size_t)
#  endif

#endif  // MALLOC_DECL

#undef NOTHROW_MALLOC_DECL
#undef MALLOC_DECL
#undef MALLOC_FUNCS
