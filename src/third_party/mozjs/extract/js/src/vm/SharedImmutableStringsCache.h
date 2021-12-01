/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_SharedImmutableStringsCache_h
#define vm_SharedImmutableStringsCache_h

#include "mozilla/Maybe.h"
#include "mozilla/UniquePtr.h"

#include <cstring>
#include <new> // for placement new

#include "builtin/String.h"

#include "js/HashTable.h"
#include "js/Utility.h"

#include "threading/ExclusiveData.h"

#include "vm/MutexIDs.h"

namespace js {

class SharedImmutableString;
class SharedImmutableTwoByteString;

/**
 * The `SharedImmutableStringsCache` allows for safely sharing and deduplicating
 * immutable strings (either `const char*` or `const char16_t*`) between
 * threads.
 *
 * The locking mechanism is dead-simple and coarse grained: a single lock guards
 * all of the internal table itself, the table's entries, and the entries'
 * reference counts. It is only safe to perform any mutation on the cache or any
 * data stored within the cache when this lock is acquired.
 */
class SharedImmutableStringsCache
{
    friend class SharedImmutableString;
    friend class SharedImmutableTwoByteString;
    struct Hasher;

  public:
    using OwnedChars = mozilla::UniquePtr<char[], JS::FreePolicy>;
    using OwnedTwoByteChars = mozilla::UniquePtr<char16_t[], JS::FreePolicy>;

    /**
     * Get the canonical, shared, and de-duplicated version of the given `const
     * char*` string. If such a string does not exist, call `intoOwnedChars` and
     * add the string it returns to the cache.
     *
     * `intoOwnedChars` must create an owned version of the given string, and
     * must have one of the following types:
     *
     *     mozilla::UniquePtr<char[], JS::FreePolicy>   intoOwnedChars();
     *     mozilla::UniquePtr<char[], JS::FreePolicy>&& intoOwnedChars();
     *
     * It can be used by callers to elide a copy of the string when it is safe
     * to give up ownership of the lookup string to the cache. It must return a
     * `nullptr` on failure.
     *
     * On success, `Some` is returned. In the case of OOM failure, `Nothing` is
     * returned.
     */
    template <typename IntoOwnedChars>
    MOZ_MUST_USE mozilla::Maybe<SharedImmutableString>
    getOrCreate(const char* chars, size_t length, IntoOwnedChars intoOwnedChars);

    /**
     * Take ownership of the given `chars` and return the canonical, shared and
     * de-duplicated version.
     *
     * On success, `Some` is returned. In the case of OOM failure, `Nothing` is
     * returned.
     */
    MOZ_MUST_USE mozilla::Maybe<SharedImmutableString>
    getOrCreate(OwnedChars&& chars, size_t length);

    /**
     * Do not take ownership of the given `chars`. Return the canonical, shared
     * and de-duplicated version. If there is no extant shared version of
     * `chars`, make a copy and insert it into the cache.
     *
     * On success, `Some` is returned. In the case of OOM failure, `Nothing` is
     * returned.
     */
    MOZ_MUST_USE mozilla::Maybe<SharedImmutableString>
    getOrCreate(const char* chars, size_t length);

    /**
     * Get the canonical, shared, and de-duplicated version of the given `const
     * char16_t*` string. If such a string does not exist, call `intoOwnedChars`
     * and add the string it returns to the cache.
     *
     * `intoOwnedTwoByteChars` must create an owned version of the given string,
     * and must have one of the following types:
     *
     *     mozilla::UniquePtr<char16_t[], JS::FreePolicy>   intoOwnedTwoByteChars();
     *     mozilla::UniquePtr<char16_t[], JS::FreePolicy>&& intoOwnedTwoByteChars();
     *
     * It can be used by callers to elide a copy of the string when it is safe
     * to give up ownership of the lookup string to the cache. It must return a
     * `nullptr` on failure.
     *
     * On success, `Some` is returned. In the case of OOM failure, `Nothing` is
     * returned.
     */
    template <typename IntoOwnedTwoByteChars>
    MOZ_MUST_USE mozilla::Maybe<SharedImmutableTwoByteString>
    getOrCreate(const char16_t* chars, size_t length, IntoOwnedTwoByteChars intoOwnedTwoByteChars);

    /**
     * Take ownership of the given `chars` and return the canonical, shared and
     * de-duplicated version.
     *
     * On success, `Some` is returned. In the case of OOM failure, `Nothing` is
     * returned.
     */
    MOZ_MUST_USE mozilla::Maybe<SharedImmutableTwoByteString>
    getOrCreate(OwnedTwoByteChars&& chars, size_t length);

    /**
     * Do not take ownership of the given `chars`. Return the canonical, shared
     * and de-duplicated version. If there is no extant shared version of
     * `chars`, then make a copy and insert it into the cache.
     *
     * On success, `Some` is returned. In the case of OOM failure, `Nothing` is
     * returned.
     */
    MOZ_MUST_USE mozilla::Maybe<SharedImmutableTwoByteString>
    getOrCreate(const char16_t* chars, size_t length);

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf) const {
        MOZ_ASSERT(inner_);
        size_t n = mallocSizeOf(inner_);

        auto locked = inner_->lock();
        if (!locked->set.initialized())
            return n;

        // Size of the table.
        n += locked->set.sizeOfExcludingThis(mallocSizeOf);

        // Sizes of the strings and their boxes.
        for (auto r = locked->set.all(); !r.empty(); r.popFront()) {
            n += mallocSizeOf(r.front().get());
            if (const char* chars = r.front()->chars())
                n += mallocSizeOf(chars);
        }

        return n;
    }

    /**
     * Construct a new cache of shared, immutable strings. Returns
     * `mozilla::Nothing` on out of memory failure.
     */
    static mozilla::Maybe<SharedImmutableStringsCache> Create() {
        auto inner = js_new<ExclusiveData<Inner>>(mutexid::SharedImmutableStringsCache);
        if (!inner)
            return mozilla::Nothing();

        auto locked = inner->lock();
        return mozilla::Some(SharedImmutableStringsCache(locked));
    }

    SharedImmutableStringsCache(SharedImmutableStringsCache&& rhs)
      : inner_(rhs.inner_)
    {
        MOZ_ASSERT(inner_);
        rhs.inner_ = nullptr;
    }

    SharedImmutableStringsCache& operator=(SharedImmutableStringsCache&& rhs) {
        MOZ_ASSERT(this != &rhs, "self move not allowed");
        new (this) SharedImmutableStringsCache(mozilla::Move(rhs));
        return *this;
    }

    SharedImmutableStringsCache& operator=(const SharedImmutableStringsCache&) = delete;

    SharedImmutableStringsCache clone() {
        MOZ_ASSERT(inner_);
        auto locked = inner_->lock();
        return SharedImmutableStringsCache(locked);
    }

    ~SharedImmutableStringsCache() {
        if (!inner_)
            return;

        bool shouldDestroy = false;
        {
            // ~ExclusiveData takes the lock, so be sure to drop the lock before
            // attempting to destroy the inner.
            auto locked = inner_->lock();
            MOZ_ASSERT(locked->refcount > 0);
            locked->refcount--;
            if (locked->refcount == 0)
                shouldDestroy = true;
        }
        if (shouldDestroy)
            js_delete(inner_);
    }

    /**
     * Purge the cache of all refcount == 0 entries.
     */
    void purge() {
        auto locked = inner_->lock();
        MOZ_ASSERT(locked->refcount > 0);

        if (!locked->set.initialized())
            return;

        for (Inner::Set::Enum e(locked->set); !e.empty(); e.popFront()) {
            if (e.front()->refcount == 0) {
                // The chars should be eagerly freed when refcount reaches zero.
                MOZ_ASSERT(!e.front()->chars());
                e.removeFront();
            } else {
                // The chars should exist as long as the refcount is non-zero.
                MOZ_ASSERT(e.front()->chars());
            }
        }
    }

  private:
    class StringBox
    {
        friend class SharedImmutableString;

        OwnedChars chars_;
        size_t length_;

      public:
        mutable size_t refcount;

        using Ptr = mozilla::UniquePtr<StringBox, JS::DeletePolicy<StringBox>>;

        StringBox(OwnedChars&& chars, size_t length)
          : chars_(mozilla::Move(chars))
          , length_(length)
          , refcount(0)
        {
            MOZ_ASSERT(chars_);
        }

        static Ptr Create(OwnedChars&& chars, size_t length) {
            return Ptr(js_new<StringBox>(mozilla::Move(chars), length));
        }

        StringBox(const StringBox&) = delete;
        StringBox& operator=(const StringBox&) = delete;

        ~StringBox() {
            MOZ_RELEASE_ASSERT(refcount == 0,
                               "There are `SharedImmutable[TwoByte]String`s outliving their "
                               "associated cache! This always leads to use-after-free in the "
                               "`~SharedImmutableString` destructor!");
        }

        const char* chars() const { return chars_.get(); }
        size_t length() const { return length_; }
    };

    struct Hasher
    {
        /**
         * A structure used when querying for a `const char*` string in the cache.
         */
        class Lookup
        {
            friend struct Hasher;

            HashNumber hash_;
            const char* chars_;
            size_t length_;

          public:
            Lookup(HashNumber hash, const char* chars, size_t length)
              : hash_(hash)
              , chars_(chars)
              , length_(length)
            {
                MOZ_ASSERT(chars_);
                MOZ_ASSERT(hash == Hasher::hashLongString(chars, length));
            }

            Lookup(HashNumber hash, const char16_t* chars, size_t length)
              : Lookup(hash, reinterpret_cast<const char*>(chars), length * sizeof(char16_t))
            { }
        };

        static const size_t SHORT_STRING_MAX_LENGTH = 8192;
        static const size_t HASH_CHUNK_LENGTH = SHORT_STRING_MAX_LENGTH / 2;

        // For strings longer than SHORT_STRING_MAX_LENGTH, we only hash the
        // first HASH_CHUNK_LENGTH and last HASH_CHUNK_LENGTH characters in the
        // string. This increases the risk of collisions, but in practice it
        // should be rare, and it yields a large speedup for hashing long
        // strings.
        static HashNumber hashLongString(const char* chars, size_t length) {
            MOZ_ASSERT(chars);
            return length <= SHORT_STRING_MAX_LENGTH
                ? mozilla::HashString(chars, length)
                : mozilla::AddToHash(mozilla::HashString(chars, HASH_CHUNK_LENGTH),
                                     mozilla::HashString(chars + length - HASH_CHUNK_LENGTH,
                                                         HASH_CHUNK_LENGTH));
        }

        static HashNumber hash(const Lookup& lookup) {
            return lookup.hash_;
        }

        static bool match(const StringBox::Ptr& key, const Lookup& lookup) {
            MOZ_ASSERT(lookup.chars_);

            if (!key->chars() || key->length() != lookup.length_)
                return false;

            if (key->chars() == lookup.chars_)
                return true;

            return memcmp(key->chars(), lookup.chars_, key->length()) == 0;
        }
    };

    // The `Inner` struct contains the actual cached contents, and is reference
    // counted and shared between all `SharedImmutableStringsCache` and
    // `SharedImmutable[TwoByte]String` holders.
    struct Inner
    {
        using Set = HashSet<StringBox::Ptr, Hasher, SystemAllocPolicy>;

        size_t refcount;
        Set set;

        Inner()
          : refcount(0)
          , set()
        { }

        Inner(const Inner&) = delete;
        Inner& operator=(const Inner&) = delete;

        ~Inner()
        {
            MOZ_ASSERT(refcount == 0);
        }
    };

    const ExclusiveData<Inner>* inner_;

    explicit SharedImmutableStringsCache(ExclusiveData<Inner>::Guard& locked)
      : inner_(locked.parent())
    {
        locked->refcount++;
    }
};

/**
 * The `SharedImmutableString` class holds a reference to a `const char*` string
 * from the `SharedImmutableStringsCache` and releases the reference upon
 * destruction.
 */
class SharedImmutableString
{
    friend class SharedImmutableStringsCache;
    friend class SharedImmutableTwoByteString;

    mutable SharedImmutableStringsCache cache_;
    mutable SharedImmutableStringsCache::StringBox* box_;

    SharedImmutableString(ExclusiveData<SharedImmutableStringsCache::Inner>::Guard& locked,
                          SharedImmutableStringsCache::StringBox* box);

  public:
    /**
     * `SharedImmutableString`s are move-able. It is an error to use a
     * `SharedImmutableString` after it has been moved.
     */
    SharedImmutableString(SharedImmutableString&& rhs);
    SharedImmutableString& operator=(SharedImmutableString&& rhs);

    /**
     * Create another shared reference to the underlying string.
     */
    SharedImmutableString clone() const;

    // If you want a copy, take one explicitly with `clone`!
    SharedImmutableString& operator=(const SharedImmutableString&) = delete;

    ~SharedImmutableString();

    /**
     * Get a raw pointer to the underlying string. It is only safe to use the
     * resulting pointer while this `SharedImmutableString` exists.
     */
    const char* chars() const {
        MOZ_ASSERT(box_);
        MOZ_ASSERT(box_->refcount > 0);
        MOZ_ASSERT(box_->chars());
        return box_->chars();
    }

    /**
     * Get the length of the underlying string.
     */
    size_t length() const {
        MOZ_ASSERT(box_);
        MOZ_ASSERT(box_->refcount > 0);
        MOZ_ASSERT(box_->chars());
        return box_->length();
    }
};

/**
 * The `SharedImmutableTwoByteString` class holds a reference to a `const
 * char16_t*` string from the `SharedImmutableStringsCache` and releases the
 * reference upon destruction.
 */
class SharedImmutableTwoByteString
{
    friend class SharedImmutableStringsCache;

    // If a `char*` string and `char16_t*` string happen to have the same bytes,
    // the bytes will be shared but handed out as different types.
    SharedImmutableString string_;

    explicit SharedImmutableTwoByteString(SharedImmutableString&& string);
    SharedImmutableTwoByteString(ExclusiveData<SharedImmutableStringsCache::Inner>::Guard& locked,
                                 SharedImmutableStringsCache::StringBox* box);

  public:
    /**
     * `SharedImmutableTwoByteString`s are move-able. It is an error to use a
     * `SharedImmutableTwoByteString` after it has been moved.
     */
    SharedImmutableTwoByteString(SharedImmutableTwoByteString&& rhs);
    SharedImmutableTwoByteString& operator=(SharedImmutableTwoByteString&& rhs);

    /**
     * Create another shared reference to the underlying string.
     */
    SharedImmutableTwoByteString clone() const;

    // If you want a copy, take one explicitly with `clone`!
    SharedImmutableTwoByteString& operator=(const SharedImmutableTwoByteString&) = delete;

    /**
     * Get a raw pointer to the underlying string. It is only safe to use the
     * resulting pointer while this `SharedImmutableTwoByteString` exists.
     */
    const char16_t* chars() const { return reinterpret_cast<const char16_t*>(string_.chars()); }

    /**
     * Get the length of the underlying string.
     */
    size_t length() const { return string_.length() / sizeof(char16_t); }
};

} // namespace js

#endif // vm_SharedImmutableStringsCache_h
