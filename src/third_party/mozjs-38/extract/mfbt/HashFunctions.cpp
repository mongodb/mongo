/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Implementations of hash functions. */

#include "mozilla/HashFunctions.h"
#include "mozilla/Types.h"

#include <string.h>

namespace mozilla {

uint32_t
HashBytes(const void* aBytes, size_t aLength)
{
  uint32_t hash = 0;
  const char* b = reinterpret_cast<const char*>(aBytes);

  /* Walk word by word. */
  size_t i = 0;
  for (; i < aLength - (aLength % sizeof(size_t)); i += sizeof(size_t)) {
    /* Do an explicitly unaligned load of the data. */
    size_t data;
    memcpy(&data, b + i, sizeof(size_t));

    hash = AddToHash(hash, data, sizeof(data));
  }

  /* Get the remaining bytes. */
  for (; i < aLength; i++) {
    hash = AddToHash(hash, b[i]);
  }
  return hash;
}

} /* namespace mozilla */
