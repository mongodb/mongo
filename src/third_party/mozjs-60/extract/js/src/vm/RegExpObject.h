/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JavaScript RegExp objects. */

#ifndef vm_RegExpObject_h
#define vm_RegExpObject_h

#include "mozilla/Attributes.h"
#include "mozilla/MemoryReporting.h"

#include "builtin/SelfHostingDefines.h"
#include "gc/Marking.h"
#include "js/GCHashTable.h"
#include "proxy/Proxy.h"
#include "vm/ArrayObject.h"
#include "vm/JSContext.h"
#include "vm/RegExpShared.h"
#include "vm/Shape.h"

namespace JS { struct Zone; }

/*
 * JavaScript Regular Expressions
 *
 * There are several engine concepts associated with a single logical regexp:
 *
 *   RegExpObject:
 *     The JS-visible object whose .[[Class]] equals "RegExp".
 *   RegExpShared:
 *     The compiled representation of the regexp (lazily created, cleared
 *     during some forms of GC).
 *   RegExpZone:
 *     Owns all RegExpShared instances in a zone.
 */
namespace js {

struct MatchPair;
class MatchPairs;
class RegExpStatics;

namespace frontend { class TokenStreamAnyChars; }

extern RegExpObject*
RegExpAlloc(JSContext* cx, NewObjectKind newKind, HandleObject proto = nullptr);

extern JSObject*
CloneRegExpObject(JSContext* cx, Handle<RegExpObject*> regex);

class RegExpObject : public NativeObject
{
    static const unsigned LAST_INDEX_SLOT          = 0;
    static const unsigned SOURCE_SLOT              = 1;
    static const unsigned FLAGS_SLOT               = 2;

    static_assert(RegExpObject::FLAGS_SLOT == REGEXP_FLAGS_SLOT,
                  "FLAGS_SLOT values should be in sync with self-hosted JS");

  public:
    static const unsigned RESERVED_SLOTS = 3;
    static const unsigned PRIVATE_SLOT = 3;

    static const Class class_;
    static const Class protoClass_;

    // The maximum number of pairs a MatchResult can have, without having to
    // allocate a bigger MatchResult.
    static const size_t MaxPairCount = 14;

    template<typename CharT>
    static RegExpObject*
    create(JSContext* cx, const CharT* chars, size_t length, RegExpFlag flags, LifoAlloc& alloc,
           NewObjectKind newKind);

    template<typename CharT>
    static RegExpObject*
    create(JSContext* cx, const CharT* chars, size_t length, RegExpFlag flags,
           frontend::TokenStreamAnyChars& ts, LifoAlloc& alloc, NewObjectKind kind);

    static RegExpObject*
    create(JSContext* cx, HandleAtom atom, RegExpFlag flags, LifoAlloc& alloc,
           NewObjectKind newKind);

    static RegExpObject*
    create(JSContext* cx, HandleAtom atom, RegExpFlag flags, frontend::TokenStreamAnyChars& ts,
           LifoAlloc& alloc, NewObjectKind newKind);

    /*
     * Compute the initial shape to associate with fresh RegExp objects,
     * encoding their initial properties. Return the shape after
     * changing |obj|'s last property to it.
     */
    static Shape*
    assignInitialShape(JSContext* cx, Handle<RegExpObject*> obj);

    /* Accessors. */

    static unsigned lastIndexSlot() { return LAST_INDEX_SLOT; }

    static bool isInitialShape(RegExpObject* rx) {
        Shape* shape = rx->lastProperty();
        if (shape->isEmptyShape() || !shape->isDataProperty())
            return false;
        if (shape->maybeSlot() != LAST_INDEX_SLOT)
            return false;
        return true;
    }

    const Value& getLastIndex() const { return getSlot(LAST_INDEX_SLOT); }

    void setLastIndex(double d) {
        setSlot(LAST_INDEX_SLOT, NumberValue(d));
    }

    void zeroLastIndex(JSContext* cx) {
        MOZ_ASSERT(lookupPure(cx->names().lastIndex)->writable(),
                   "can't infallibly zero a non-writable lastIndex on a "
                   "RegExp that's been exposed to script");
        setSlot(LAST_INDEX_SLOT, Int32Value(0));
    }

    JSFlatString* toString(JSContext* cx) const;

    JSAtom* getSource() const { return &getSlot(SOURCE_SLOT).toString()->asAtom(); }

    void setSource(JSAtom* source) {
        setSlot(SOURCE_SLOT, StringValue(source));
    }

    /* Flags. */

    static unsigned flagsSlot() { return FLAGS_SLOT; }

    RegExpFlag getFlags() const {
        return RegExpFlag(getFixedSlot(FLAGS_SLOT).toInt32());
    }
    void setFlags(RegExpFlag flags) {
        setSlot(FLAGS_SLOT, Int32Value(flags));
    }

    bool ignoreCase() const { return getFlags() & IgnoreCaseFlag; }
    bool global() const     { return getFlags() & GlobalFlag; }
    bool multiline() const  { return getFlags() & MultilineFlag; }
    bool sticky() const     { return getFlags() & StickyFlag; }
    bool unicode() const    { return getFlags() & UnicodeFlag; }

    static bool isOriginalFlagGetter(JSNative native, RegExpFlag* mask);

    static RegExpShared* getShared(JSContext* cx, Handle<RegExpObject*> regexp);

    bool hasShared() {
        return !!sharedRef();
    }

    void setShared(RegExpShared& shared) {
        MOZ_ASSERT(!hasShared());
        sharedRef().init(&shared);
    }

    PreBarriered<RegExpShared*>& sharedRef() {
        auto& ref = NativeObject::privateRef(PRIVATE_SLOT);
        return reinterpret_cast<PreBarriered<RegExpShared*>&>(ref);
    }

    static void trace(JSTracer* trc, JSObject* obj);
    void trace(JSTracer* trc);

    void initIgnoringLastIndex(JSAtom* source, RegExpFlag flags);

    // NOTE: This method is *only* safe to call on RegExps that haven't been
    //       exposed to script, because it requires that the "lastIndex"
    //       property be writable.
    void initAndZeroLastIndex(JSAtom* source, RegExpFlag flags, JSContext* cx);

#ifdef DEBUG
    static MOZ_MUST_USE bool dumpBytecode(JSContext* cx, Handle<RegExpObject*> regexp,
                                          bool match_only, HandleLinearString input);
#endif

  private:
    /*
     * Precondition: the syntax for |source| has already been validated.
     * Side effect: sets the private field.
     */
    static RegExpShared* createShared(JSContext* cx, Handle<RegExpObject*> regexp);

    /* Call setShared in preference to setPrivate. */
    void setPrivate(void* priv) = delete;
};

/*
 * Parse regexp flags. Report an error and return false if an invalid
 * sequence of flags is encountered (repeat/invalid flag).
 *
 * N.B. flagStr must be rooted.
 */
bool
ParseRegExpFlags(JSContext* cx, JSString* flagStr, RegExpFlag* flagsOut);

/* Assuming GetBuiltinClass(obj) is ESClass::RegExp, return a RegExpShared for obj. */
inline RegExpShared*
RegExpToShared(JSContext* cx, HandleObject obj)
{
    if (obj->is<RegExpObject>())
        return RegExpObject::getShared(cx, obj.as<RegExpObject>());

    return Proxy::regexp_toShared(cx, obj);
}

template<XDRMode mode>
bool
XDRScriptRegExpObject(XDRState<mode>* xdr, MutableHandle<RegExpObject*> objp);

extern JSObject*
CloneScriptRegExpObject(JSContext* cx, RegExpObject& re);

/* Escape all slashes and newlines in the given string. */
extern JSAtom*
EscapeRegExpPattern(JSContext* cx, HandleAtom src);

template <typename CharT>
extern bool
HasRegExpMetaChars(const CharT* chars, size_t length);

extern bool
StringHasRegExpMetaChars(JSLinearString* str);

} /* namespace js */

#endif /* vm_RegExpObject_h */
