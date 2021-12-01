/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Represents the native path format on the platform. */

#ifndef mozilla_Path_h
#define mozilla_Path_h

namespace mozilla {
namespace filesystem {

/*
 * Mozilla vaiant of std::filesystem::path.
 * Only |value_type| is implemented at the moment.
 */
class Path
{
public:
#ifdef XP_WIN
  using value_type = char16_t;
#else
  using value_type = char;
#endif
};

}  /* namespace filesystem */
}  /* namespace mozilla */

#endif /* mozilla_Path_h */
