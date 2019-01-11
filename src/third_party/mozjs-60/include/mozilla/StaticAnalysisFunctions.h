/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_StaticAnalysisFunctions_h
#define mozilla_StaticAnalysisFunctions_h

#ifndef __cplusplus
#ifndef bool
#include <stdbool.h>
#endif
#endif
/*
 * Functions that are used as markers in Gecko code for static analysis. Their
 * purpose is to have different AST nodes generated during compile time and to
 * match them based on different checkers implemented in build/clang-plugin
 */

#ifdef MOZ_CLANG_PLUGIN

#ifdef __cplusplus
/**
 * MOZ_KnownLive - used to opt an argument out of the CanRunScript checker so
 * that we don't check it if is a strong ref.
 *
 * Example:
 * canRunScript(MOZ_KnownLive(rawPointer));
 */
template <typename T>
static MOZ_ALWAYS_INLINE T* MOZ_KnownLive(T* ptr) { return ptr; }

extern "C" {
#endif

/**
 * MOZ_AssertAssignmentTest - used in MOZ_ASSERT in order to test the possible
 * presence of assignment instead of logical comparisons.
 *
 * Example:
 * MOZ_ASSERT(retVal = true);
 */
static MOZ_ALWAYS_INLINE bool MOZ_AssertAssignmentTest(bool exprResult) {
  return exprResult;
}

#ifdef __cplusplus
}
#endif /* __cplusplus */

#define MOZ_CHECK_ASSERT_ASSIGNMENT(expr) MOZ_AssertAssignmentTest(!!(expr))

#else

#define MOZ_CHECK_ASSERT_ASSIGNMENT(expr) (!!(expr))
#define MOZ_KnownLive(expr) (expr)

#endif /* MOZ_CLANG_PLUGIN */
#endif /* StaticAnalysisFunctions_h */
