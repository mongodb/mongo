/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Checking for jsrust crate availability for linking.
 * For testing, define MOZ_PRETEND_NO_JSRUST to pretend
 * that we don't have jsrust.
 */

#ifndef mozilla_JsRust_h
#define mozilla_JsRust_h

#define MOZ_PRETEND_NO_JSRUST 1  // Avoid all Rust dependencies.

#if (defined(MOZ_HAS_MOZGLUE) || defined(MOZILLA_INTERNAL_API)) && \
    !defined(MOZ_PRETEND_NO_JSRUST)
#  define MOZ_HAS_JSRUST() 1
#else
#  define MOZ_HAS_JSRUST() 0
#endif

#endif  // mozilla_JsRust_h
