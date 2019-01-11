/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_SharedImmutableStringsCache_inl_h
#define vm_SharedImmutableStringsCache_inl_h

#include "vm/SharedImmutableStringsCache.h"

namespace js {

template <typename IntoOwnedChars>
MOZ_MUST_USE mozilla::Maybe<SharedImmutableString>
SharedImmutableStringsCache::getOrCreate(const char* chars, size_t length,
                                         IntoOwnedChars intoOwnedChars)
{
    MOZ_ASSERT(inner_);
    MOZ_ASSERT(chars);
    Hasher::Lookup lookup(Hasher::hashLongString(chars, length), chars, length);

    auto locked = inner_->lock();
    if (!locked->set.initialized() && !locked->set.init())
        return mozilla::Nothing();

    auto entry = locked->set.lookupForAdd(lookup);
    if (!entry) {
        OwnedChars ownedChars(intoOwnedChars());
        if (!ownedChars)
            return mozilla::Nothing();
        MOZ_ASSERT(ownedChars.get() == chars ||
                   memcmp(ownedChars.get(), chars, length) == 0);
        auto box = StringBox::Create(mozilla::Move(ownedChars), length);
        if (!box || !locked->set.add(entry, mozilla::Move(box)))
            return mozilla::Nothing();
    }

    MOZ_ASSERT(entry && *entry);
    return mozilla::Some(SharedImmutableString(locked, entry->get()));
}

template <typename IntoOwnedTwoByteChars>
MOZ_MUST_USE mozilla::Maybe<SharedImmutableTwoByteString>
SharedImmutableStringsCache::getOrCreate(const char16_t* chars, size_t length,
                                         IntoOwnedTwoByteChars intoOwnedTwoByteChars) {
    MOZ_ASSERT(inner_);
    MOZ_ASSERT(chars);
    auto hash = Hasher::hashLongString(reinterpret_cast<const char*>(chars),
                                       length * sizeof(char16_t));
    Hasher::Lookup lookup(hash, chars, length);

    auto locked = inner_->lock();
    if (!locked->set.initialized() && !locked->set.init())
        return mozilla::Nothing();

    auto entry = locked->set.lookupForAdd(lookup);
    if (!entry) {
        OwnedTwoByteChars ownedTwoByteChars(intoOwnedTwoByteChars());
        if (!ownedTwoByteChars)
            return mozilla::Nothing();
        MOZ_ASSERT(ownedTwoByteChars.get() == chars ||
                   memcmp(ownedTwoByteChars.get(), chars, length * sizeof(char16_t)) == 0);
        OwnedChars ownedChars(reinterpret_cast<char*>(ownedTwoByteChars.release()));
        auto box = StringBox::Create(mozilla::Move(ownedChars), length * sizeof(char16_t));
        if (!box || !locked->set.add(entry, mozilla::Move(box)))
            return mozilla::Nothing();
    }

    MOZ_ASSERT(entry && *entry);
    return mozilla::Some(SharedImmutableTwoByteString(locked, entry->get()));
}

} // namespace js

#endif // vm_SharedImmutableStringsCache_inl_h
