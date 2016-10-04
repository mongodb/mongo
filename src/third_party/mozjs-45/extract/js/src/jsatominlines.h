/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsatominlines_h
#define jsatominlines_h

#include "jsatom.h"

#include "mozilla/PodOperations.h"
#include "mozilla/RangedPtr.h"

#include "jscntxt.h"
#include "jsnum.h"

#include "vm/String.h"

inline JSAtom*
js::AtomStateEntry::asPtr() const
{
    MOZ_ASSERT(bits != 0);
    JSAtom* atom = reinterpret_cast<JSAtom*>(bits & NO_TAG_MASK);
    JSString::readBarrier(atom);
    return atom;
}

inline JSAtom*
js::AtomStateEntry::asPtrUnbarriered() const
{
    MOZ_ASSERT(bits != 0);
    return reinterpret_cast<JSAtom*>(bits & NO_TAG_MASK);
}

namespace js {

inline jsid
AtomToId(JSAtom* atom)
{
    JS_STATIC_ASSERT(JSID_INT_MIN == 0);

    uint32_t index;
    if (atom->isIndex(&index) && index <= JSID_INT_MAX)
        return INT_TO_JSID(int32_t(index));

    return JSID_FROM_BITS(size_t(atom));
}

inline bool
ValueToIdPure(const Value& v, jsid* id)
{
    int32_t i;
    if (ValueFitsInInt32(v, &i) && INT_FITS_IN_JSID(i)) {
        *id = INT_TO_JSID(i);
        return true;
    }

    if (js::IsSymbolOrSymbolWrapper(v)) {
        *id = SYMBOL_TO_JSID(js::ToSymbolPrimitive(v));
        return true;
    }

    if (!v.isString() || !v.toString()->isAtom())
        return false;

    *id = AtomToId(&v.toString()->asAtom());
    return true;
}

template <AllowGC allowGC>
inline bool
ValueToId(ExclusiveContext* cx, typename MaybeRooted<Value, allowGC>::HandleType v,
          typename MaybeRooted<jsid, allowGC>::MutableHandleType idp)
{
    int32_t i;
    if (ValueFitsInInt32(v, &i) && INT_FITS_IN_JSID(i)) {
        idp.set(INT_TO_JSID(i));
        return true;
    }

    if (js::IsSymbolOrSymbolWrapper(v)) {
        idp.set(SYMBOL_TO_JSID(js::ToSymbolPrimitive(v)));
        return true;
    }

    JSAtom* atom = ToAtom<allowGC>(cx, v);
    if (!atom)
        return false;

    idp.set(AtomToId(atom));
    return true;
}

/*
 * Write out character representing |index| to the memory just before |end|.
 * Thus |*end| is not touched, but |end[-1]| and earlier are modified as
 * appropriate.  There must be at least js::UINT32_CHAR_BUFFER_LENGTH elements
 * before |end| to avoid buffer underflow.  The start of the characters written
 * is returned and is necessarily before |end|.
 */
template <typename T>
inline mozilla::RangedPtr<T>
BackfillIndexInCharBuffer(uint32_t index, mozilla::RangedPtr<T> end)
{
#ifdef DEBUG
    /*
     * Assert that the buffer we're filling will hold as many characters as we
     * could write out, by dereferencing the index that would hold the most
     * significant digit.
     */
    (void) *(end - UINT32_CHAR_BUFFER_LENGTH);
#endif

    do {
        uint32_t next = index / 10, digit = index % 10;
        *--end = '0' + digit;
        index = next;
    } while (index > 0);

    return end;
}

bool
IndexToIdSlow(ExclusiveContext* cx, uint32_t index, MutableHandleId idp);

inline bool
IndexToId(ExclusiveContext* cx, uint32_t index, MutableHandleId idp)
{
    if (index <= JSID_INT_MAX) {
        idp.set(INT_TO_JSID(index));
        return true;
    }

    return IndexToIdSlow(cx, index, idp);
}

static MOZ_ALWAYS_INLINE JSFlatString*
IdToString(JSContext* cx, jsid id)
{
    if (JSID_IS_STRING(id))
        return JSID_TO_ATOM(id);

    if (MOZ_LIKELY(JSID_IS_INT(id)))
        return Int32ToString<CanGC>(cx, JSID_TO_INT(id));

    RootedValue idv(cx, IdToValue(id));
    JSString* str = ToStringSlow<CanGC>(cx, idv);
    if (!str)
        return nullptr;

    return str->ensureFlat(cx);
}

inline
AtomHasher::Lookup::Lookup(const JSAtom* atom)
  : isLatin1(atom->hasLatin1Chars()), length(atom->length()), atom(atom)
{
    if (isLatin1) {
        latin1Chars = atom->latin1Chars(nogc);
        hash = mozilla::HashString(latin1Chars, length);
    } else {
        twoByteChars = atom->twoByteChars(nogc);
        hash = mozilla::HashString(twoByteChars, length);
    }
}

inline bool
AtomHasher::match(const AtomStateEntry& entry, const Lookup& lookup)
{
    JSAtom* key = entry.asPtr();
    if (lookup.atom)
        return lookup.atom == key;
    if (key->length() != lookup.length)
        return false;

    if (key->hasLatin1Chars()) {
        const Latin1Char* keyChars = key->latin1Chars(lookup.nogc);
        if (lookup.isLatin1)
            return mozilla::PodEqual(keyChars, lookup.latin1Chars, lookup.length);
        return EqualChars(keyChars, lookup.twoByteChars, lookup.length);
    }

    const char16_t* keyChars = key->twoByteChars(lookup.nogc);
    if (lookup.isLatin1)
        return EqualChars(lookup.latin1Chars, keyChars, lookup.length);
    return mozilla::PodEqual(keyChars, lookup.twoByteChars, lookup.length);
}

inline Handle<PropertyName*>
TypeName(JSType type, const JSAtomState& names)
{
    MOZ_ASSERT(type < JSTYPE_LIMIT);
    JS_STATIC_ASSERT(offsetof(JSAtomState, undefined) +
                     JSTYPE_LIMIT * sizeof(ImmutablePropertyNamePtr) <=
                     sizeof(JSAtomState));
    JS_STATIC_ASSERT(JSTYPE_VOID == 0);
    return (&names.undefined)[type];
}

inline Handle<PropertyName*>
ClassName(JSProtoKey key, JSAtomState& atomState)
{
    MOZ_ASSERT(key < JSProto_LIMIT);
    JS_STATIC_ASSERT(offsetof(JSAtomState, Null) +
                     JSProto_LIMIT * sizeof(ImmutablePropertyNamePtr) <=
                     sizeof(JSAtomState));
    JS_STATIC_ASSERT(JSProto_Null == 0);
    return (&atomState.Null)[key];
}

inline Handle<PropertyName*>
ClassName(JSProtoKey key, JSRuntime* rt)
{
    return ClassName(key, *rt->commonNames);
}

inline Handle<PropertyName*>
ClassName(JSProtoKey key, ExclusiveContext* cx)
{
    return ClassName(key, cx->names());
}

} // namespace js

#endif /* jsatominlines_h */
