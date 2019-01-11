/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * JS atom table.
 */

#include "vm/JSAtom-inl.h"

#include "mozilla/ArrayUtils.h"
#include "mozilla/EndianUtils.h"
#include "mozilla/RangedPtr.h"
#include "mozilla/Unused.h"

#include <string.h>

#include "jstypes.h"

#include "builtin/String.h"
#include "gc/Marking.h"
#include "util/Text.h"
#include "vm/JSContext.h"
#include "vm/SymbolType.h"
#include "vm/Xdr.h"

#include "gc/AtomMarking-inl.h"
#include "vm/JSCompartment-inl.h"
#include "vm/JSContext-inl.h"
#include "vm/JSObject-inl.h"
#include "vm/StringType-inl.h"

using namespace js;
using namespace js::gc;

using mozilla::ArrayEnd;
using mozilla::ArrayLength;
using mozilla::Maybe;
using mozilla::Nothing;
using mozilla::RangedPtr;

struct js::AtomHasher::Lookup
{
    union {
        const JS::Latin1Char* latin1Chars;
        const char16_t* twoByteChars;
    };
    bool isLatin1;
    size_t length;
    const JSAtom* atom; /* Optional. */
    JS::AutoCheckCannotGC nogc;

    HashNumber hash;

    MOZ_ALWAYS_INLINE Lookup(const char16_t* chars, size_t length)
      : twoByteChars(chars), isLatin1(false), length(length), atom(nullptr),
        hash(mozilla::HashString(chars, length))
    {}

    MOZ_ALWAYS_INLINE Lookup(const JS::Latin1Char* chars, size_t length)
      : latin1Chars(chars), isLatin1(true), length(length), atom(nullptr),
        hash(mozilla::HashString(chars, length))
    {}

    inline explicit Lookup(const JSAtom* atom)
      : isLatin1(atom->hasLatin1Chars()), length(atom->length()), atom(atom),
        hash(atom->hash())
    {
        if (isLatin1) {
            latin1Chars = atom->latin1Chars(nogc);
            MOZ_ASSERT(mozilla::HashString(latin1Chars, length) == hash);
        } else {
            twoByteChars = atom->twoByteChars(nogc);
            MOZ_ASSERT(mozilla::HashString(twoByteChars, length) == hash);
        }
    }
};

inline HashNumber
js::AtomHasher::hash(const Lookup& l)
{
    return l.hash;
}

MOZ_ALWAYS_INLINE bool
js::AtomHasher::match(const AtomStateEntry& entry, const Lookup& lookup)
{
    JSAtom* key = entry.asPtrUnbarriered();
    if (lookup.atom)
        return lookup.atom == key;
    if (key->length() != lookup.length || key->hash() != lookup.hash)
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

inline JSAtom*
js::AtomStateEntry::asPtr(JSContext* cx) const
{
    JSAtom* atom = asPtrUnbarriered();
    if (!cx->helperThread())
        JSString::readBarrier(atom);
    return atom;
}

const char*
js::AtomToPrintableString(JSContext* cx, JSAtom* atom, JSAutoByteString* bytes)
{
    JSString* str = QuoteString(cx, atom, 0);
    if (!str)
        return nullptr;
    return bytes->encodeLatin1(cx, str);
}

#define DEFINE_PROTO_STRING(name,init,clasp) const char js_##name##_str[] = #name;
JS_FOR_EACH_PROTOTYPE(DEFINE_PROTO_STRING)
#undef DEFINE_PROTO_STRING

#define CONST_CHAR_STR(idpart, id, text) const char js_##idpart##_str[] = text;
FOR_EACH_COMMON_PROPERTYNAME(CONST_CHAR_STR)
#undef CONST_CHAR_STR

/* Constant strings that are not atomized. */
const char js_getter_str[]          = "getter";
const char js_send_str[]            = "send";
const char js_setter_str[]          = "setter";

// Use a low initial capacity for atom hash tables to avoid penalizing runtimes
// which create a small number of atoms.
static const uint32_t JS_STRING_HASH_COUNT = 64;

MOZ_ALWAYS_INLINE AtomSet::Ptr
js::FrozenAtomSet::readonlyThreadsafeLookup(const AtomSet::Lookup& l) const
{
    return mSet->readonlyThreadsafeLookup(l);
}

struct CommonNameInfo
{
    const char* str;
    size_t length;
};

bool
JSRuntime::initializeAtoms(JSContext* cx)
{
    atoms_ = js_new<AtomSet>();
    if (!atoms_ || !atoms_->init(JS_STRING_HASH_COUNT))
        return false;

    // |permanentAtoms| hasn't been created yet.
    MOZ_ASSERT(!permanentAtoms);

    if (parentRuntime) {
        staticStrings = parentRuntime->staticStrings;
        commonNames = parentRuntime->commonNames;
        emptyString = parentRuntime->emptyString;
        permanentAtoms = parentRuntime->permanentAtoms;
        wellKnownSymbols = parentRuntime->wellKnownSymbols;
        return true;
    }

    staticStrings = js_new<StaticStrings>();
    if (!staticStrings || !staticStrings->init(cx))
        return false;

    static const CommonNameInfo cachedNames[] = {
#define COMMON_NAME_INFO(idpart, id, text) { js_##idpart##_str, sizeof(text) - 1 },
        FOR_EACH_COMMON_PROPERTYNAME(COMMON_NAME_INFO)
#undef COMMON_NAME_INFO
#define COMMON_NAME_INFO(name, init, clasp) { js_##name##_str, sizeof(#name) - 1 },
        JS_FOR_EACH_PROTOTYPE(COMMON_NAME_INFO)
#undef COMMON_NAME_INFO
#define COMMON_NAME_INFO(name) { #name, sizeof(#name) - 1 },
        JS_FOR_EACH_WELL_KNOWN_SYMBOL(COMMON_NAME_INFO)
#undef COMMON_NAME_INFO
#define COMMON_NAME_INFO(name) { "Symbol." #name, sizeof("Symbol." #name) - 1 },
        JS_FOR_EACH_WELL_KNOWN_SYMBOL(COMMON_NAME_INFO)
#undef COMMON_NAME_INFO
    };

    commonNames = js_new<JSAtomState>();
    if (!commonNames)
        return false;

    ImmutablePropertyNamePtr* names = reinterpret_cast<ImmutablePropertyNamePtr*>(commonNames.ref());
    for (size_t i = 0; i < ArrayLength(cachedNames); i++, names++) {
        JSAtom* atom = Atomize(cx, cachedNames[i].str, cachedNames[i].length, PinAtom);
        if (!atom)
            return false;
        names->init(atom->asPropertyName());
    }
    MOZ_ASSERT(uintptr_t(names) == uintptr_t(commonNames + 1));

    emptyString = commonNames->empty;

    // Create the well-known symbols.
    wellKnownSymbols = js_new<WellKnownSymbols>();
    if (!wellKnownSymbols)
        return false;

    ImmutablePropertyNamePtr* descriptions = commonNames->wellKnownSymbolDescriptions();
    ImmutableSymbolPtr* symbols = reinterpret_cast<ImmutableSymbolPtr*>(wellKnownSymbols.ref());
    for (size_t i = 0; i < JS::WellKnownSymbolLimit; i++) {
        JS::Symbol* symbol = JS::Symbol::new_(cx, JS::SymbolCode(i), descriptions[i]);
        if (!symbol) {
            ReportOutOfMemory(cx);
            return false;
        }
        symbols[i].init(symbol);
    }

    return true;
}

void
JSRuntime::finishAtoms()
{
    js_delete(atoms_.ref());

    if (!parentRuntime) {
        js_delete(staticStrings.ref());
        js_delete(commonNames.ref());
        js_delete(permanentAtoms.ref());
        js_delete(wellKnownSymbols.ref());
    }

    atoms_ = nullptr;
    staticStrings = nullptr;
    commonNames = nullptr;
    permanentAtoms = nullptr;
    wellKnownSymbols = nullptr;
    emptyString = nullptr;
}

static inline void
TracePinnedAtoms(JSTracer* trc, const AtomSet& atoms)
{
    for (auto r = atoms.all(); !r.empty(); r.popFront()) {
        const AtomStateEntry& entry = r.front();
        if (entry.isPinned()) {
            JSAtom* atom = entry.asPtrUnbarriered();
            TraceRoot(trc, &atom, "interned_atom");
            MOZ_ASSERT(entry.asPtrUnbarriered() == atom);
        }
    }
}

void
js::TraceAtoms(JSTracer* trc, AutoLockForExclusiveAccess& lock)
{
    JSRuntime* rt = trc->runtime();

    if (rt->atomsAreFinished())
        return;

    TracePinnedAtoms(trc, rt->atoms(lock));
    if (rt->atomsAddedWhileSweeping())
        TracePinnedAtoms(trc, *rt->atomsAddedWhileSweeping());
}

void
js::TracePermanentAtoms(JSTracer* trc)
{
    JSRuntime* rt = trc->runtime();

    // Permanent atoms only need to be traced in the runtime which owns them.
    if (rt->parentRuntime)
        return;

    // Static strings are not included in the permanent atoms table.
    if (rt->staticStrings)
        rt->staticStrings->trace(trc);

    if (rt->permanentAtoms) {
        for (FrozenAtomSet::Range r(rt->permanentAtoms->all()); !r.empty(); r.popFront()) {
            const AtomStateEntry& entry = r.front();

            JSAtom* atom = entry.asPtrUnbarriered();
            TraceProcessGlobalRoot(trc, atom, "permanent_table");
        }
    }
}

void
js::TraceWellKnownSymbols(JSTracer* trc)
{
    JSRuntime* rt = trc->runtime();

    if (rt->parentRuntime)
        return;

    if (WellKnownSymbols* wks = rt->wellKnownSymbols) {
        for (size_t i = 0; i < JS::WellKnownSymbolLimit; i++)
            TraceProcessGlobalRoot(trc, wks->get(i).get(), "well_known_symbol");
    }
}

bool
JSRuntime::transformToPermanentAtoms(JSContext* cx)
{
    MOZ_ASSERT(!parentRuntime);

    // All static strings were created as permanent atoms, now move the contents
    // of the atoms table into permanentAtoms and mark each as permanent.

    MOZ_ASSERT(!permanentAtoms);
    permanentAtoms = js_new<FrozenAtomSet>(atoms_);   // takes ownership of atoms_

    atoms_ = js_new<AtomSet>();
    if (!atoms_ || !atoms_->init(JS_STRING_HASH_COUNT))
        return false;

    for (FrozenAtomSet::Range r(permanentAtoms->all()); !r.empty(); r.popFront()) {
        AtomStateEntry entry = r.front();
        JSAtom* atom = entry.asPtr(cx);
        atom->morphIntoPermanentAtom();
    }

    return true;
}

static inline AtomSet::Ptr
LookupAtomState(JSRuntime* rt, const AtomHasher::Lookup& lookup)
{
    MOZ_ASSERT(rt->currentThreadHasExclusiveAccess());

    AtomSet::Ptr p = rt->unsafeAtoms().lookup(lookup); // Safe because we hold the lock.
    if (!p && rt->atomsAddedWhileSweeping())
        p = rt->atomsAddedWhileSweeping()->lookup(lookup);
    return p;
}

bool
AtomIsPinned(JSContext* cx, JSAtom* atom)
{
    /* We treat static strings as interned because they're never collected. */
    if (StaticStrings::isStatic(atom))
        return true;

    AtomHasher::Lookup lookup(atom);

    /* Likewise, permanent strings are considered to be interned. */
    MOZ_ASSERT(cx->isPermanentAtomsInitialized());
    AtomSet::Ptr p = cx->permanentAtoms().readonlyThreadsafeLookup(lookup);
    if (p)
        return true;

    AutoLockForExclusiveAccess lock(cx);

    p = LookupAtomState(cx->runtime(), lookup);
    if (!p)
        return false;

    return p->isPinned();
}

#ifdef DEBUG

bool
AtomIsPinnedInRuntime(JSRuntime* rt, JSAtom* atom)
{
    Maybe<AutoLockForExclusiveAccess> lock;
    if (!rt->currentThreadHasExclusiveAccess())
        lock.emplace(rt);

    AtomHasher::Lookup lookup(atom);

    AtomSet::Ptr p = LookupAtomState(rt, lookup);
    MOZ_ASSERT(p);

    return p->isPinned();
}

#endif // DEBUG

template <typename CharT>
MOZ_ALWAYS_INLINE
static JSAtom*
AtomizeAndCopyCharsInner(JSContext* cx, const CharT* tbchars, size_t length, PinningBehavior pin,
                         const Maybe<uint32_t>& indexValue, const AtomHasher::Lookup& lookup);

/* |tbchars| must not point into an inline or short string. */
template <typename CharT>
MOZ_ALWAYS_INLINE
static JSAtom*
AtomizeAndCopyChars(JSContext* cx, const CharT* tbchars, size_t length, PinningBehavior pin,
                    const Maybe<uint32_t>& indexValue)
{
    if (JSAtom* s = cx->staticStrings().lookup(tbchars, length))
        return s;

    AtomHasher::Lookup lookup(tbchars, length);

    // Try the per-Zone cache first. If we find the atom there we can avoid the
    // atoms lock, the markAtom call, and the multiple HashSet lookups below.
    // We don't use the per-Zone cache if we want a pinned atom: handling that
    // is more complicated and pinning atoms is relatively uncommon.
    Zone* zone = cx->zone();
    Maybe<AtomSet::AddPtr> zonePtr;
    if (MOZ_LIKELY(zone && pin == DoNotPinAtom)) {
        zonePtr.emplace(zone->atomCache().lookupForAdd(lookup));
        if (zonePtr.ref()) {
            // The cache is purged on GC so if we're in the middle of an
            // incremental GC we should have barriered the atom when we put
            // it in the cache.
            JSAtom* atom = zonePtr.ref()->asPtrUnbarriered();
            MOZ_ASSERT(AtomIsMarked(zone, atom));
            return atom;
        }
    }

    // Note: when this function is called while the permanent atoms table is
    // being initialized (in initializeAtoms()), |permanentAtoms| is not yet
    // initialized so this lookup is always skipped. Only once
    // transformToPermanentAtoms() is called does |permanentAtoms| get
    // initialized and then this lookup will go ahead.
    if (cx->isPermanentAtomsInitialized()) {
        AtomSet::Ptr pp = cx->permanentAtoms().readonlyThreadsafeLookup(lookup);
        if (pp) {
            JSAtom* atom = pp->asPtr(cx);
            if (zonePtr)
                mozilla::Unused << zone->atomCache().add(*zonePtr, AtomStateEntry(atom, false));
            return atom;
        }
    }

    // Validate the length before taking the exclusive access lock, as throwing
    // an exception here may reenter this code.
    if (MOZ_UNLIKELY(!JSString::validateLength(cx, length)))
        return nullptr;

    JSAtom* atom = AtomizeAndCopyCharsInner(cx, tbchars, length, pin, indexValue, lookup);
    if (!atom)
        return nullptr;

    cx->atomMarking().inlinedMarkAtom(cx, atom);

    if (zonePtr)
        mozilla::Unused << zone->atomCache().add(*zonePtr, AtomStateEntry(atom, false));

    return atom;
}

template <typename CharT>
MOZ_ALWAYS_INLINE
static JSAtom*
AtomizeAndCopyCharsInner(JSContext* cx, const CharT* tbchars, size_t length, PinningBehavior pin,
                         const Maybe<uint32_t>& indexValue, const AtomHasher::Lookup& lookup)
{
    AutoLockForExclusiveAccess lock(cx);

    JSRuntime* rt = cx->runtime();
    AtomSet& atoms = rt->atoms(lock);
    AtomSet* atomsAddedWhileSweeping = rt->atomsAddedWhileSweeping();
    AtomSet::AddPtr p;

    if (!atomsAddedWhileSweeping) {
        p = atoms.lookupForAdd(lookup);
    } else {
        // We're currently sweeping the main atoms table and all new atoms will
        // be added to a secondary table. Check this first.
        MOZ_ASSERT(rt->atomsZone(lock)->isGCSweeping());
        p = atomsAddedWhileSweeping->lookupForAdd(lookup);

        // If that fails check the main table but check if any atom found there
        // is dead.
        if (!p) {
            if (AtomSet::AddPtr p2 = atoms.lookupForAdd(lookup)) {
                JSAtom* atom = p2->asPtrUnbarriered();
                if (!IsAboutToBeFinalizedUnbarriered(&atom))
                    p = p2;
            }
        }
    }

    if (p) {
        JSAtom* atom = p->asPtr(cx);
        p->setPinned(bool(pin));
        return atom;
    }

    JSAtom* atom;
    {
        AutoAtomsCompartment ac(cx, lock);

        JSFlatString* flat = NewStringCopyN<NoGC>(cx, tbchars, length);
        if (!flat) {
            // Grudgingly forgo last-ditch GC. The alternative would be to release
            // the lock, manually GC here, and retry from the top. If you fix this,
            // please also fix or comment the similar case in Symbol::new_.
            ReportOutOfMemory(cx);
            return nullptr;
        }

        atom = flat->morphAtomizedStringIntoAtom(lookup.hash);
        MOZ_ASSERT(atom->hash() == lookup.hash);

        if (indexValue)
            atom->maybeInitializeIndex(*indexValue, true);

        // We have held the lock since looking up p, and the operations we've done
        // since then can't GC; therefore the atoms table has not been modified and
        // p is still valid.
        AtomSet* addSet = atomsAddedWhileSweeping ? atomsAddedWhileSweeping : &atoms;
        if (!addSet->add(p, AtomStateEntry(atom, bool(pin)))) {
            ReportOutOfMemory(cx); /* SystemAllocPolicy does not report OOM. */
            return nullptr;
        }
    }

    return atom;
}

template JSAtom*
AtomizeAndCopyChars(JSContext* cx, const char16_t* tbchars, size_t length, PinningBehavior pin,
                    const Maybe<uint32_t>& indexValue);

template JSAtom*
AtomizeAndCopyChars(JSContext* cx, const Latin1Char* tbchars, size_t length, PinningBehavior pin,
                    const Maybe<uint32_t>& indexValue);

JSAtom*
js::AtomizeString(JSContext* cx, JSString* str,
                  js::PinningBehavior pin /* = js::DoNotPinAtom */)
{
    if (str->isAtom()) {
        JSAtom& atom = str->asAtom();
        /* N.B. static atoms are effectively always interned. */
        if (pin != PinAtom || js::StaticStrings::isStatic(&atom))
            return &atom;

        AtomHasher::Lookup lookup(&atom);

        /* Likewise, permanent atoms are always interned. */
        MOZ_ASSERT(cx->isPermanentAtomsInitialized());
        AtomSet::Ptr p = cx->permanentAtoms().readonlyThreadsafeLookup(lookup);
        if (p)
            return &atom;

        AutoLockForExclusiveAccess lock(cx);

        p = LookupAtomState(cx->runtime(), lookup);
        MOZ_ASSERT(p); /* Non-static atom must exist in atom state set. */
        MOZ_ASSERT(p->asPtrUnbarriered() == &atom);
        MOZ_ASSERT(pin == PinAtom);
        p->setPinned(bool(pin));
        return &atom;
    }

    JSLinearString* linear = str->ensureLinear(cx);
    if (!linear)
        return nullptr;

    Maybe<uint32_t> indexValue;
    if (str->hasIndexValue())
        indexValue.emplace(str->getIndexValue());

    JS::AutoCheckCannotGC nogc;
    return linear->hasLatin1Chars()
           ? AtomizeAndCopyChars(cx, linear->latin1Chars(nogc), linear->length(), pin, indexValue)
           : AtomizeAndCopyChars(cx, linear->twoByteChars(nogc), linear->length(), pin, indexValue);
}

JSAtom*
js::Atomize(JSContext* cx, const char* bytes, size_t length, PinningBehavior pin,
            const Maybe<uint32_t>& indexValue)
{
    CHECK_REQUEST(cx);

    const Latin1Char* chars = reinterpret_cast<const Latin1Char*>(bytes);
    return AtomizeAndCopyChars(cx, chars, length, pin, indexValue);
}

template <typename CharT>
JSAtom*
js::AtomizeChars(JSContext* cx, const CharT* chars, size_t length, PinningBehavior pin)
{
    CHECK_REQUEST(cx);
    return AtomizeAndCopyChars(cx, chars, length, pin, Nothing());
}

template JSAtom*
js::AtomizeChars(JSContext* cx, const Latin1Char* chars, size_t length, PinningBehavior pin);

template JSAtom*
js::AtomizeChars(JSContext* cx, const char16_t* chars, size_t length, PinningBehavior pin);

JSAtom*
js::AtomizeUTF8Chars(JSContext* cx, const char* utf8Chars, size_t utf8ByteLength)
{
    // This could be optimized to hand the char16_t's directly to the JSAtom
    // instead of making a copy. UTF8CharsToNewTwoByteCharsZ should be
    // refactored to take an JSContext so that this function could also.

    UTF8Chars utf8(utf8Chars, utf8ByteLength);

    size_t length;
    UniqueTwoByteChars chars(JS::UTF8CharsToNewTwoByteCharsZ(cx, utf8, &length).get());
    if (!chars)
        return nullptr;

    return AtomizeChars(cx, chars.get(), length);
}

bool
js::IndexToIdSlow(JSContext* cx, uint32_t index, MutableHandleId idp)
{
    MOZ_ASSERT(index > JSID_INT_MAX);

    char16_t buf[UINT32_CHAR_BUFFER_LENGTH];
    RangedPtr<char16_t> end(ArrayEnd(buf), buf, ArrayEnd(buf));
    RangedPtr<char16_t> start = BackfillIndexInCharBuffer(index, end);

    JSAtom* atom = AtomizeChars(cx, start.get(), end - start);
    if (!atom)
        return false;

    idp.set(JSID_FROM_BITS((size_t)atom));
    return true;
}

template <AllowGC allowGC>
static JSAtom*
ToAtomSlow(JSContext* cx, typename MaybeRooted<Value, allowGC>::HandleType arg)
{
    MOZ_ASSERT(!arg.isString());

    Value v = arg;
    if (!v.isPrimitive()) {
        MOZ_ASSERT(!cx->helperThread());
        if (!allowGC)
            return nullptr;
        RootedValue v2(cx, v);
        if (!ToPrimitive(cx, JSTYPE_STRING, &v2))
            return nullptr;
        v = v2;
    }

    if (v.isString()) {
        JSAtom* atom = AtomizeString(cx, v.toString());
        if (!allowGC && !atom)
            cx->recoverFromOutOfMemory();
        return atom;
    }
    if (v.isInt32()) {
        JSAtom* atom = Int32ToAtom(cx, v.toInt32());
        if (!allowGC && !atom)
            cx->recoverFromOutOfMemory();
        return atom;
    }
    if (v.isDouble()) {
        JSAtom* atom = NumberToAtom(cx, v.toDouble());
        if (!allowGC && !atom)
            cx->recoverFromOutOfMemory();
        return atom;
    }
    if (v.isBoolean())
        return v.toBoolean() ? cx->names().true_ : cx->names().false_;
    if (v.isNull())
        return cx->names().null;
    if (v.isSymbol()) {
        MOZ_ASSERT(!cx->helperThread());
        if (allowGC) {
            JS_ReportErrorNumberASCII(cx, GetErrorMessage, nullptr,
                                      JSMSG_SYMBOL_TO_STRING);
        }
        return nullptr;
    }
    MOZ_ASSERT(v.isUndefined());
    return cx->names().undefined;
}

template <AllowGC allowGC>
JSAtom*
js::ToAtom(JSContext* cx, typename MaybeRooted<Value, allowGC>::HandleType v)
{
    if (!v.isString())
        return ToAtomSlow<allowGC>(cx, v);

    JSString* str = v.toString();
    if (str->isAtom())
        return &str->asAtom();

    JSAtom* atom = AtomizeString(cx, str);
    if (!atom && !allowGC) {
        MOZ_ASSERT_IF(!cx->helperThread(), cx->isThrowingOutOfMemory());
        cx->recoverFromOutOfMemory();
    }
    return atom;
}

template JSAtom*
js::ToAtom<CanGC>(JSContext* cx, HandleValue v);

template JSAtom*
js::ToAtom<NoGC>(JSContext* cx, const Value& v);

template<XDRMode mode>
bool
js::XDRAtom(XDRState<mode>* xdr, MutableHandleAtom atomp)
{
    if (mode == XDR_ENCODE) {
        static_assert(JSString::MAX_LENGTH <= INT32_MAX, "String length must fit in 31 bits");
        uint32_t length = atomp->length();
        uint32_t lengthAndEncoding = (length << 1) | uint32_t(atomp->hasLatin1Chars());
        if (!xdr->codeUint32(&lengthAndEncoding))
            return false;

        JS::AutoCheckCannotGC nogc;
        return atomp->hasLatin1Chars()
               ? xdr->codeChars(atomp->latin1Chars(nogc), length)
               : xdr->codeChars(const_cast<char16_t*>(atomp->twoByteChars(nogc)), length);
    }

    /* Avoid JSString allocation for already existing atoms. See bug 321985. */
    uint32_t lengthAndEncoding;
    if (!xdr->codeUint32(&lengthAndEncoding))
        return false;

    uint32_t length = lengthAndEncoding >> 1;
    bool latin1 = lengthAndEncoding & 0x1;

    JSContext* cx = xdr->cx();
    JSAtom* atom;
    if (latin1) {
        const Latin1Char* chars = nullptr;
        if (length) {
            const uint8_t *ptr;
            size_t nbyte = length * sizeof(Latin1Char);
            if (!xdr->peekData(&ptr, nbyte))
                return false;
            chars = reinterpret_cast<const Latin1Char*>(ptr);
        }
        atom = AtomizeChars(cx, chars, length);
    } else {
#if MOZ_LITTLE_ENDIAN
        /* Directly access the little endian chars in the XDR buffer. */
        const char16_t* chars = nullptr;
        if (length) {
            const uint8_t *ptr;
            size_t nbyte = length * sizeof(char16_t);
            if (!xdr->peekData(&ptr, nbyte))
                return false;
            chars = reinterpret_cast<const char16_t*>(ptr);
        }
        atom = AtomizeChars(cx, chars, length);
#else
        /*
         * We must copy chars to a temporary buffer to convert between little and
         * big endian data.
         */
        char16_t* chars;
        char16_t stackChars[256];
        if (length <= ArrayLength(stackChars)) {
            chars = stackChars;
        } else {
            /*
             * This is very uncommon. Don't use the tempLifoAlloc arena for this as
             * most allocations here will be bigger than tempLifoAlloc's default
             * chunk size.
             */
            chars = cx->pod_malloc<char16_t>(length);
            if (!chars)
                return false;
        }

        JS_ALWAYS_TRUE(xdr->codeChars(chars, length));
        atom = AtomizeChars(cx, chars, length);
        if (chars != stackChars)
            js_free(chars);
#endif /* !MOZ_LITTLE_ENDIAN */
    }

    if (!atom)
        return false;
    atomp.set(atom);
    return true;
}

template bool
js::XDRAtom(XDRState<XDR_ENCODE>* xdr, MutableHandleAtom atomp);

template bool
js::XDRAtom(XDRState<XDR_DECODE>* xdr, MutableHandleAtom atomp);

Handle<PropertyName*>
js::ClassName(JSProtoKey key, JSContext* cx)
{
    return ClassName(key, cx->names());
}

void
js::gc::MergeAtomsAddedWhileSweeping(JSRuntime* rt)
{
    // Add atoms that were added to the secondary table while we were sweeping
    // the main table.

    AutoEnterOOMUnsafeRegion oomUnsafe;
    AtomSet* atomsTable = rt->atomsForSweeping();
    MOZ_ASSERT(atomsTable);
    for (auto r = rt->atomsAddedWhileSweeping()->all(); !r.empty(); r.popFront()) {
        if (!atomsTable->putNew(AtomHasher::Lookup(r.front().asPtrUnbarriered()), r.front()))
            oomUnsafe.crash("Adding atom from secondary table after sweep");
    }
}
