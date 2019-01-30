/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/SharedImmutableStringsCache-inl.h"

#include "builtin/String.h"
#include "util/Text.h"

namespace js {

SharedImmutableString::SharedImmutableString(
    ExclusiveData<SharedImmutableStringsCache::Inner>::Guard& locked,
    SharedImmutableStringsCache::StringBox* box)
  : cache_(locked)
  , box_(box)
{
    MOZ_ASSERT(box);
    box->refcount++;
}

SharedImmutableString::SharedImmutableString(SharedImmutableString&& rhs)
  : cache_(mozilla::Move(rhs.cache_))
  , box_(rhs.box_)
{
    MOZ_ASSERT(this != &rhs, "self move not allowed");
    MOZ_ASSERT(rhs.box_);
    MOZ_ASSERT(rhs.box_->refcount > 0);

    rhs.box_ = nullptr;
}

SharedImmutableString&
SharedImmutableString::operator=(SharedImmutableString&& rhs) {
    this->~SharedImmutableString();
    new (this) SharedImmutableString(mozilla::Move(rhs));
    return *this;
}

SharedImmutableTwoByteString::SharedImmutableTwoByteString(SharedImmutableString&& string)
  : string_(mozilla::Move(string))
{ }

SharedImmutableTwoByteString::SharedImmutableTwoByteString(
    ExclusiveData<SharedImmutableStringsCache::Inner>::Guard& locked,
    SharedImmutableStringsCache::StringBox* box)
  : string_(locked, box)
{
    MOZ_ASSERT(box->length() % sizeof(char16_t) == 0);
}

SharedImmutableTwoByteString::SharedImmutableTwoByteString(SharedImmutableTwoByteString&& rhs)
  : string_(mozilla::Move(rhs.string_))
{
    MOZ_ASSERT(this != &rhs, "self move not allowed");
}

SharedImmutableTwoByteString&
SharedImmutableTwoByteString::operator=(SharedImmutableTwoByteString&& rhs)
{
    this->~SharedImmutableTwoByteString();
    new (this) SharedImmutableTwoByteString(mozilla::Move(rhs));
    return *this;
}

SharedImmutableString::~SharedImmutableString() {
    if (!box_)
        return;

    auto locked = cache_.inner_->lock();

    MOZ_ASSERT(box_->refcount > 0);

    box_->refcount--;
    if (box_->refcount == 0)
        box_->chars_.reset(nullptr);
}

SharedImmutableString
SharedImmutableString::clone() const
{
    auto locked = cache_.inner_->lock();
    MOZ_ASSERT(box_);
    MOZ_ASSERT(box_->refcount > 0);
    return SharedImmutableString(locked, box_);
}

SharedImmutableTwoByteString
SharedImmutableTwoByteString::clone() const
{
    return SharedImmutableTwoByteString(string_.clone());
}

MOZ_MUST_USE mozilla::Maybe<SharedImmutableString>
SharedImmutableStringsCache::getOrCreate(OwnedChars&& chars, size_t length)
{
    OwnedChars owned(mozilla::Move(chars));
    MOZ_ASSERT(owned);
    return getOrCreate(owned.get(), length, [&]() { return mozilla::Move(owned); });
}

MOZ_MUST_USE mozilla::Maybe<SharedImmutableString>
SharedImmutableStringsCache::getOrCreate(const char* chars, size_t length)
{
    return getOrCreate(chars, length, [&]() { return DuplicateString(chars, length); });
}

MOZ_MUST_USE mozilla::Maybe<SharedImmutableTwoByteString>
SharedImmutableStringsCache::getOrCreate(OwnedTwoByteChars&& chars, size_t length)
{
    OwnedTwoByteChars owned(mozilla::Move(chars));
    MOZ_ASSERT(owned);
    return getOrCreate(owned.get(), length, [&]() { return mozilla::Move(owned); });
}

MOZ_MUST_USE mozilla::Maybe<SharedImmutableTwoByteString>
SharedImmutableStringsCache::getOrCreate(const char16_t* chars, size_t length)
{
    return getOrCreate(chars, length, [&]() { return DuplicateString(chars, length); });
}

} // namespace js
