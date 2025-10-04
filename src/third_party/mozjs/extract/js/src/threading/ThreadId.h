/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef threading_ThreadId_h
#define threading_ThreadId_h

namespace js {

class ThreadId {
  class PlatformData;
  void* platformData_[2];

 public:
  ThreadId();

  ThreadId(const ThreadId&) = default;
  ThreadId(ThreadId&&) = default;
  ThreadId& operator=(const ThreadId&) = default;
  ThreadId& operator=(ThreadId&&) = default;

  bool operator==(const ThreadId& aOther) const;
  bool operator!=(const ThreadId& aOther) const { return !operator==(aOther); }

  MOZ_IMPLICIT operator bool() const;

  inline PlatformData* platformData();
  inline const PlatformData* platformData() const;

  // Return the thread id of the calling thread.
  static ThreadId ThisThreadId();
};

}  // namespace js

#endif  // threading_ThreadId_h
