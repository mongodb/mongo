/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_SharedImmutableStringsCache_inl_h
#define vm_SharedImmutableStringsCache_inl_h

#include "vm/SharedImmutableStringsCache.h"

namespace js {

template <typename IntoOwnedChars>
[[nodiscard]] SharedImmutableString SharedImmutableStringsCache::getOrCreate(
    const char* chars, size_t length, IntoOwnedChars intoOwnedChars) {
  MOZ_ASSERT(inner_);
  MOZ_ASSERT(chars);
  Hasher::Lookup lookup(Hasher::hashLongString(chars, length), chars, length);

  auto locked = inner_->lock();
  auto entry = locked->set.lookupForAdd(lookup);
  if (!entry) {
    OwnedChars ownedChars(intoOwnedChars());
    if (!ownedChars) {
      return SharedImmutableString();
    }
    MOZ_ASSERT(ownedChars.get() == chars ||
               memcmp(ownedChars.get(), chars, length) == 0);
    auto box = StringBox::Create(std::move(ownedChars), length, inner_);
    if (!box || !locked->set.add(entry, std::move(box))) {
      return SharedImmutableString();
    }
  }

  MOZ_ASSERT(entry && *entry);
  return SharedImmutableString(entry->get());
}

template <typename IntoOwnedTwoByteChars>
[[nodiscard]] SharedImmutableTwoByteString
SharedImmutableStringsCache::getOrCreate(
    const char16_t* chars, size_t length,
    IntoOwnedTwoByteChars intoOwnedTwoByteChars) {
  MOZ_ASSERT(inner_);
  MOZ_ASSERT(chars);
  auto hash = Hasher::hashLongString(reinterpret_cast<const char*>(chars),
                                     length * sizeof(char16_t));
  Hasher::Lookup lookup(hash, chars, length);

  auto locked = inner_->lock();
  auto entry = locked->set.lookupForAdd(lookup);
  if (!entry) {
    OwnedTwoByteChars ownedTwoByteChars(intoOwnedTwoByteChars());
    if (!ownedTwoByteChars) {
      return SharedImmutableTwoByteString();
    }
    MOZ_ASSERT(
        ownedTwoByteChars.get() == chars ||
        memcmp(ownedTwoByteChars.get(), chars, length * sizeof(char16_t)) == 0);
    OwnedChars ownedChars(reinterpret_cast<char*>(ownedTwoByteChars.release()));
    auto box = StringBox::Create(std::move(ownedChars),
                                 length * sizeof(char16_t), inner_);
    if (!box || !locked->set.add(entry, std::move(box))) {
      return SharedImmutableTwoByteString();
    }
  }

  MOZ_ASSERT(entry && *entry);
  return SharedImmutableTwoByteString(entry->get());
}

}  // namespace js

#endif  // vm_SharedImmutableStringsCache_inl_h
