/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_RefCountType_h
#define mozilla_RefCountType_h

#include <stdint.h>

/**
 * MozRefCountType is Mozilla's reference count type.
 *
 * We use the same type to represent the refcount of RefCounted objects
 * as well, in order to be able to use the leak detection facilities
 * that are implemented by XPCOM.
 *
 * Note that this type is not in the mozilla namespace so that it is
 * usable for both C and C++ code.
 */
typedef uintptr_t MozRefCountType;

/*
 * This is the return type for AddRef() and Release() in nsISupports.
 * IUnknown of COM returns an unsigned long from equivalent functions.
 *
 * The following ifdef exists to maintain binary compatibility with
 * IUnknown, the base interface in Microsoft COM.
 */
#ifdef XP_WIN
typedef unsigned long MozExternalRefCountType;
#else
typedef uint32_t MozExternalRefCountType;
#endif

#endif
