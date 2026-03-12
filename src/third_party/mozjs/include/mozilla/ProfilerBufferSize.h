/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ProfilerBufferSize_h
#define ProfilerBufferSize_h

#include "mozilla/ProfileBufferChunkManager.h"

// We need to decide how many chunks of what size we want to fit in the given
// total maximum capacity for this process, in the (likely) context of
// multiple processes doing the same choice and having an inter-process
// mechanism to control the overall memory limit.

// The buffer size is provided as a number of "entries", this is their size in
// bytes.
constexpr static uint32_t scBytesPerEntry = 8;

// Minimum chunk size allowed, enough for at least one stack.
constexpr static uint32_t scMinimumChunkSize =
    2 * mozilla::ProfileBufferChunkManager::scExpectedMaximumStackSize;

// Ideally we want at least 2 unreleased chunks to work with (1 current and 1
// next), and 2 released chunks (so that one can be recycled when old, leaving
// one with some data).
constexpr static uint32_t scMinimumNumberOfChunks = 4;

// And we want to limit chunks to a maximum size, which is a compromise
// between:
// - A big size, which helps with reducing the rate of allocations and IPCs.
// - A small size, which helps with equalizing the duration of recorded data
//   (as the inter-process controller will discard the oldest chunks in all
//   Firefox processes).
constexpr static uint32_t scMaximumChunkSize = 1024 * 1024;

// Limit to 128MiB as a lower buffer size usually isn't enough.
constexpr static uint32_t scMinimumBufferSize = 128u * 1024u * 1024u;
// Note: Keep in sync with GeckoThread.maybeStartGeckoProfiler:
// https://searchfox.org/mozilla-central/source/mobile/android/geckoview/src/main/java/org/mozilla/gecko/GeckoThread.java
constexpr static uint32_t scMinimumBufferEntries =
    scMinimumBufferSize / scBytesPerEntry;

// Limit to 2GiB.
constexpr static uint32_t scMaximumBufferSize = 2u * 1024u * 1024u * 1024u;
constexpr static uint32_t scMaximumBufferEntries =
    scMaximumBufferSize / scBytesPerEntry;

constexpr static uint32_t ClampToAllowedEntries(uint32_t aEntries) {
  if (aEntries <= scMinimumBufferEntries) {
    return scMinimumBufferEntries;
  }
  if (aEntries >= scMaximumBufferEntries) {
    return scMaximumBufferEntries;
  }
  return aEntries;
}

#endif  // ProfilerBufferSize_h
