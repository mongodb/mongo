/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* APIs for getting a stack trace of an arbitrary thread */

#ifndef mozilla_StackWalkThread_h
#define mozilla_StackWalkThread_h

#include "mozilla/StackWalk.h"

#include <windows.h>

/**
 * Like MozStackWalk, but walks the stack for another thread.
 * Call aCallback for each stack frame on the current thread, from
 * the caller of MozStackWalk to main (or above).
 *
 * @param aCallback    Same as for MozStackWalk().
 * @param aMaxFrames   Same as for MozStackWalk().
 * @param aClosure     Same as for MozStackWalk().
 * @param aThread      The handle of the thread whose stack is to be walked.
 *                     If 0, walks the current thread.
 * @param aContext     A CONTEXT, presumably obtained with GetThreadContext()
 *                     after suspending the thread with SuspendThread(). If
 *                     null, the CONTEXT will be re-obtained.
 */
MFBT_API void MozStackWalkThread(MozWalkStackCallback aCallback,
                                 uint32_t aMaxFrames, void* aClosure,
                                 HANDLE aThread, CONTEXT* aContext);

#endif
