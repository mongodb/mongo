/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* MOZ_STRINGIFY Macros */

#ifndef mozilla_HelperMacros_h
#define mozilla_HelperMacros_h

// Wraps x in quotes without expanding a macro name
#define MOZ_STRINGIFY_NO_EXPANSION(x) #x

// Wraps x in quotes; expanding x if it as a macro name
#define MOZ_STRINGIFY(x) MOZ_STRINGIFY_NO_EXPANSION(x)

#endif  // mozilla_HelperMacros_h
