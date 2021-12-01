/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_JSAtom_h
#define vm_JSAtom_h

#include "mozilla/Maybe.h"

#include "gc/Rooting.h"
#include "js/TypeDecls.h"
#include "vm/CommonPropertyNames.h"

class JSAutoByteString;

namespace js {

/*
 * Return a printable, lossless char[] representation of a string-type atom.
 * The lifetime of the result matches the lifetime of bytes.
 */
extern const char*
AtomToPrintableString(JSContext* cx, JSAtom* atom, JSAutoByteString* bytes);

class PropertyName;

}  /* namespace js */

extern bool
AtomIsPinned(JSContext* cx, JSAtom* atom);

#ifdef DEBUG

// This may be called either with or without the atoms lock held.
extern bool
AtomIsPinnedInRuntime(JSRuntime* rt, JSAtom* atom);

#endif // DEBUG

/* Well-known predefined C strings. */
#define DECLARE_PROTO_STR(name,init,clasp) extern const char js_##name##_str[];
JS_FOR_EACH_PROTOTYPE(DECLARE_PROTO_STR)
#undef DECLARE_PROTO_STR

#define DECLARE_CONST_CHAR_STR(idpart, id, text)  extern const char js_##idpart##_str[];
FOR_EACH_COMMON_PROPERTYNAME(DECLARE_CONST_CHAR_STR)
#undef DECLARE_CONST_CHAR_STR

/* Constant strings that are not atomized. */
extern const char js_getter_str[];
extern const char js_send_str[];
extern const char js_setter_str[];

namespace js {

class AutoLockForExclusiveAccess;

/*
 * Atom tracing and garbage collection hooks.
 */
void
TraceAtoms(JSTracer* trc, AutoLockForExclusiveAccess& lock);

void
TracePermanentAtoms(JSTracer* trc);

void
TraceWellKnownSymbols(JSTracer* trc);

/* N.B. must correspond to boolean tagging behavior. */
enum PinningBehavior
{
    DoNotPinAtom = false,
    PinAtom = true
};

extern JSAtom*
Atomize(JSContext* cx, const char* bytes, size_t length,
        js::PinningBehavior pin = js::DoNotPinAtom,
        const mozilla::Maybe<uint32_t>& indexValue = mozilla::Nothing());

template <typename CharT>
extern JSAtom*
AtomizeChars(JSContext* cx, const CharT* chars, size_t length,
             js::PinningBehavior pin = js::DoNotPinAtom);

extern JSAtom*
AtomizeUTF8Chars(JSContext* cx, const char* utf8Chars, size_t utf8ByteLength);

extern JSAtom*
AtomizeString(JSContext* cx, JSString* str, js::PinningBehavior pin = js::DoNotPinAtom);

template <AllowGC allowGC>
extern JSAtom*
ToAtom(JSContext* cx, typename MaybeRooted<JS::Value, allowGC>::HandleType v);

enum XDRMode {
    XDR_ENCODE,
    XDR_DECODE
};

template <XDRMode mode>
class XDRState;

template<XDRMode mode>
bool
XDRAtom(XDRState<mode>* xdr, js::MutableHandleAtom atomp);

extern JS::Handle<PropertyName*>
ClassName(JSProtoKey key, JSContext* cx);

namespace gc {
void MergeAtomsAddedWhileSweeping(JSRuntime* rt);
} // namespace gc

#ifdef DEBUG

bool AtomIsMarked(JS::Zone* zone, JSAtom* atom);
bool AtomIsMarked(JS::Zone* zone, jsid id);
bool AtomIsMarked(JS::Zone* zone, const JS::Value& value);

#endif // DEBUG

} /* namespace js */

#endif /* vm_JSAtom_h */
