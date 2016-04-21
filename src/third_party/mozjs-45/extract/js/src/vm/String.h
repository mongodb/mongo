/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_String_h
#define vm_String_h

#include "mozilla/MemoryReporting.h"
#include "mozilla/PodOperations.h"
#include "mozilla/Range.h"

#include "jsapi.h"
#include "jsfriendapi.h"
#include "jsstr.h"

#include "gc/Barrier.h"
#include "gc/Heap.h"
#include "gc/Marking.h"
#include "gc/Rooting.h"
#include "js/CharacterEncoding.h"
#include "js/GCAPI.h"
#include "js/RootingAPI.h"

class JSDependentString;
class JSExtensibleString;
class JSExternalString;
class JSInlineString;
class JSRope;

namespace js {

class AutoStableStringChars;
class StaticStrings;
class PropertyName;

/* The buffer length required to contain any unsigned 32-bit integer. */
static const size_t UINT32_CHAR_BUFFER_LENGTH = sizeof("4294967295") - 1;

} /* namespace js */

/*
 * JavaScript strings
 *
 * Conceptually, a JS string is just an array of chars and a length. This array
 * of chars may or may not be null-terminated and, if it is, the null character
 * is not included in the length.
 *
 * To improve performance of common operations, the following optimizations are
 * made which affect the engine's representation of strings:
 *
 *  - The plain vanilla representation is a "flat" string which consists of a
 *    string header in the GC heap and a malloc'd null terminated char array.
 *
 *  - To avoid copying a substring of an existing "base" string , a "dependent"
 *    string (JSDependentString) can be created which points into the base
 *    string's char array.
 *
 *  - To avoid O(n^2) char buffer copying, a "rope" node (JSRope) can be created
 *    to represent a delayed string concatenation. Concatenation (called
 *    flattening) is performed if and when a linear char array is requested. In
 *    general, ropes form a binary dag whose internal nodes are JSRope string
 *    headers with no associated char array and whose leaf nodes are either flat
 *    or dependent strings.
 *
 *  - To avoid copying the leftmost string when flattening, we may produce an
 *    "extensible" string, which tracks not only its actual length but also its
 *    buffer's overall size. If such an "extensible" string appears as the
 *    leftmost string in a subsequent flatten, and its buffer has enough unused
 *    space, we can simply flatten the rest of the ropes into its buffer,
 *    leaving its text in place. We then transfer ownership of its buffer to the
 *    flattened rope, and mutate the donor extensible string into a dependent
 *    string referencing its original buffer.
 *
 *    (The term "extensible" does not imply that we ever 'realloc' the buffer.
 *    Extensible strings may have dependent strings pointing into them, and the
 *    JSAPI hands out pointers to flat strings' buffers, so resizing with
 *    'realloc' is generally not possible.)
 *
 *  - To avoid allocating small char arrays, short strings can be stored inline
 *    in the string header (JSInlineString). These come in two flavours:
 *    JSThinInlineString, which is the same size as JSString; and
 *    JSFatInlineString, which has a larger header and so can fit more chars.
 *
 *  - To avoid comparing O(n) string equality comparison, strings can be
 *    canonicalized to "atoms" (JSAtom) such that there is a single atom with a
 *    given (length,chars).
 *
 *  - To avoid copying all strings created through the JSAPI, an "external"
 *    string (JSExternalString) can be created whose chars are managed by the
 *    JSAPI client.
 *
 *  - To avoid using two bytes per character for every string, string characters
 *    are stored as Latin1 instead of TwoByte if all characters are representable
 *    in Latin1.
 *
 * Although all strings share the same basic memory layout, we can conceptually
 * arrange them into a hierarchy of operations/invariants and represent this
 * hierarchy in C++ with classes:
 *
 * C++ type                     operations+fields / invariants+properties
 * ==========================   =========================================
 * JSString (abstract)          get(Latin1|TwoByte)CharsZ, get(Latin1|TwoByte)Chars, length / -
 *  | \
 *  | JSRope                    leftChild, rightChild / -
 *  |
 * JSLinearString (abstract)    latin1Chars, twoByteChars / might be null-terminated
 *  | \
 *  | JSDependentString         base / -
 *  |
 * JSFlatString                 - / null terminated
 *  |  |
 *  |  +-- JSExternalString     - / char array memory managed by embedding
 *  |  |
 *  |  +-- JSExtensibleString   tracks total buffer capacity (including current text)
 *  |  |
 *  |  +-- JSUndependedString   original dependent base / -
 *  |  |
 *  |  +-- JSInlineString (abstract)    - / chars stored in header
 *  |      |
 *  |      +-- JSThinInlineString       - / header is normal
 *  |      |
 *  |      +-- JSFatInlineString        - / header is fat
 *  |
 * JSAtom                       - / string equality === pointer equality
 *  |
 * js::PropertyName             - / chars don't contain an index (uint32_t)
 *
 * Classes marked with (abstract) above are not literally C++ Abstract Base
 * Classes (since there are no virtual functions, pure or not, in this
 * hierarchy), but have the same meaning: there are no strings with this type as
 * its most-derived type.
 *
 * Atoms can additionally be permanent, i.e. unable to be collected, and can
 * be combined with other string types to create additional most-derived types
 * that satisfy the invariants of more than one of the abovementioned
 * most-derived types:
 *  - InlineAtom     = JSInlineString     + JSAtom (atom with inline chars, abstract)
 *  - ThinInlineAtom = JSThinInlineString + JSAtom (atom with inline chars)
 *  - FatInlineAtom  = JSFatInlineString  + JSAtom (atom with (more) inline chars)
 *
 * Derived string types can be queried from ancestor types via isX() and
 * retrieved with asX() debug-only-checked casts.
 *
 * The ensureX() operations mutate 'this' in place to effectively the type to be
 * at least X (e.g., ensureLinear will change a JSRope to be a JSFlatString).
 */

class JSString : public js::gc::TenuredCell
{
  protected:
    static const size_t NUM_INLINE_CHARS_LATIN1   = 2 * sizeof(void*) / sizeof(JS::Latin1Char);
    static const size_t NUM_INLINE_CHARS_TWO_BYTE = 2 * sizeof(void*) / sizeof(char16_t);

    /* Fields only apply to string types commented on the right. */
    struct Data
    {
        union {
            struct {
                uint32_t           flags;               /* JSString */
                uint32_t           length;              /* JSString */
            };
            uintptr_t              flattenData;         /* JSRope (temporary while flattening) */
        } u1;
        union {
            union {
                /* JS(Fat)InlineString */
                JS::Latin1Char     inlineStorageLatin1[NUM_INLINE_CHARS_LATIN1];
                char16_t           inlineStorageTwoByte[NUM_INLINE_CHARS_TWO_BYTE];
            };
            struct {
                union {
                    const JS::Latin1Char* nonInlineCharsLatin1; /* JSLinearString, except JS(Fat)InlineString */
                    const char16_t* nonInlineCharsTwoByte;/* JSLinearString, except JS(Fat)InlineString */
                    JSString*      left;               /* JSRope */
                } u2;
                union {
                    JSLinearString* base;               /* JS(Dependent|Undepended)String */
                    JSString*      right;              /* JSRope */
                    size_t         capacity;            /* JSFlatString (extensible) */
                    const JSStringFinalizer* externalFinalizer;/* JSExternalString */
                } u3;
            } s;
        };
    } d;

  public:
    /* Flags exposed only for jits */

    /*
     * The Flags Word
     *
     * The flags word stores both the string's type and its character encoding.
     *
     * If LATIN1_CHARS_BIT is set, the string's characters are stored as Latin1
     * instead of TwoByte. This flag can also be set for ropes, if both the
     * left and right nodes are Latin1. Flattening will result in a Latin1
     * string in this case.
     *
     * The other flags store the string's type. Instead of using a dense index
     * to represent the most-derived type, string types are encoded to allow
     * single-op tests for hot queries (isRope, isDependent, isFlat, isAtom)
     * which, in view of subtyping, would require slower
     * (isX() || isY() || isZ()).
     *
     * The string type encoding can be summarized as follows. The "instance
     * encoding" entry for a type specifies the flag bits used to create a
     * string instance of that type. Abstract types have no instances and thus
     * have no such entry. The "subtype predicate" entry for a type specifies
     * the predicate used to query whether a JSString instance is subtype
     * (reflexively) of that type.
     *
     *   String        Instance     Subtype
     *   type          encoding     predicate
     *   ------------------------------------
     *   Rope          000000       000000
     *   Linear        -           !000000
     *   HasBase       -            xxxx1x
     *   Dependent     000010       000010
     *   Flat          -            xxxxx1
     *   Undepended    000011       000011
     *   Extensible    010001       010001
     *   Inline        000101       xxx1xx
     *   FatInline     010101       x1x1xx
     *   External      100001       100001
     *   Atom          001001       xx1xxx
     *   PermanentAtom 101001       1x1xxx
     *   InlineAtom    -            xx11xx
     *   FatInlineAtom -            x111xx
     *
     * Note that the first 4 flag bits (from right to left in the previous table)
     * have the following meaning and can be used for some hot queries:
     *
     *   Bit 0: IsFlat
     *   Bit 1: HasBase (Dependent, Undepended)
     *   Bit 2: IsInline (Inline, FatInline)
     *   Bit 3: IsAtom (Atom, PermanentAtom)
     *
     *  "HasBase" here refers to the two string types that have a 'base' field:
     *  JSDependentString and JSUndependedString.
     *  A JSUndependedString is a JSDependentString which has been 'fixed' (by ensureFixed)
     *  to be null-terminated.  In such cases, the string must keep marking its base since
     *  there may be any number of *other* JSDependentStrings transitively depending on it.
     *
     */

    static const uint32_t FLAT_BIT               = JS_BIT(0);
    static const uint32_t HAS_BASE_BIT           = JS_BIT(1);
    static const uint32_t INLINE_CHARS_BIT       = JS_BIT(2);
    static const uint32_t ATOM_BIT               = JS_BIT(3);

    static const uint32_t ROPE_FLAGS             = 0;
    static const uint32_t DEPENDENT_FLAGS        = HAS_BASE_BIT;
    static const uint32_t UNDEPENDED_FLAGS       = FLAT_BIT | HAS_BASE_BIT;
    static const uint32_t EXTENSIBLE_FLAGS       = FLAT_BIT | JS_BIT(4);
    static const uint32_t EXTERNAL_FLAGS         = FLAT_BIT | JS_BIT(5);

    static const uint32_t FAT_INLINE_MASK        = INLINE_CHARS_BIT | JS_BIT(4);
    static const uint32_t PERMANENT_ATOM_MASK    = ATOM_BIT | JS_BIT(5);

    /* Initial flags for thin inline and fat inline strings. */
    static const uint32_t INIT_THIN_INLINE_FLAGS = FLAT_BIT | INLINE_CHARS_BIT;
    static const uint32_t INIT_FAT_INLINE_FLAGS  = FLAT_BIT | FAT_INLINE_MASK;

    static const uint32_t TYPE_FLAGS_MASK        = JS_BIT(6) - 1;

    static const uint32_t LATIN1_CHARS_BIT       = JS_BIT(6);

    static const uint32_t MAX_LENGTH             = js::MaxStringLength;

    static const JS::Latin1Char MAX_LATIN1_CHAR = 0xff;

    /*
     * Helper function to validate that a string of a given length is
     * representable by a JSString. An allocation overflow is reported if false
     * is returned.
     */
    static inline bool validateLength(js::ExclusiveContext* maybecx, size_t length);

    static void staticAsserts() {
        static_assert(JSString::MAX_LENGTH < UINT32_MAX, "Length must fit in 32 bits");
        static_assert(sizeof(JSString) ==
                      (offsetof(JSString, d.inlineStorageLatin1) +
                       NUM_INLINE_CHARS_LATIN1 * sizeof(char)),
                      "Inline Latin1 chars must fit in a JSString");
        static_assert(sizeof(JSString) ==
                      (offsetof(JSString, d.inlineStorageTwoByte) +
                       NUM_INLINE_CHARS_TWO_BYTE * sizeof(char16_t)),
                      "Inline char16_t chars must fit in a JSString");

        /* Ensure js::shadow::String has the same layout. */
        using js::shadow::String;
        static_assert(offsetof(JSString, d.u1.length) == offsetof(String, length),
                      "shadow::String length offset must match JSString");
        static_assert(offsetof(JSString, d.u1.flags) == offsetof(String, flags),
                      "shadow::String flags offset must match JSString");
        static_assert(offsetof(JSString, d.s.u2.nonInlineCharsLatin1) == offsetof(String, nonInlineCharsLatin1),
                      "shadow::String nonInlineChars offset must match JSString");
        static_assert(offsetof(JSString, d.s.u2.nonInlineCharsTwoByte) == offsetof(String, nonInlineCharsTwoByte),
                      "shadow::String nonInlineChars offset must match JSString");
        static_assert(offsetof(JSString, d.inlineStorageLatin1) == offsetof(String, inlineStorageLatin1),
                      "shadow::String inlineStorage offset must match JSString");
        static_assert(offsetof(JSString, d.inlineStorageTwoByte) == offsetof(String, inlineStorageTwoByte),
                      "shadow::String inlineStorage offset must match JSString");
        static_assert(INLINE_CHARS_BIT == String::INLINE_CHARS_BIT,
                      "shadow::String::INLINE_CHARS_BIT must match JSString::INLINE_CHARS_BIT");
        static_assert(LATIN1_CHARS_BIT == String::LATIN1_CHARS_BIT,
                      "shadow::String::LATIN1_CHARS_BIT must match JSString::LATIN1_CHARS_BIT");
        static_assert(TYPE_FLAGS_MASK == String::TYPE_FLAGS_MASK,
                      "shadow::String::TYPE_FLAGS_MASK must match JSString::TYPE_FLAGS_MASK");
        static_assert(ROPE_FLAGS == String::ROPE_FLAGS,
                      "shadow::String::ROPE_FLAGS must match JSString::ROPE_FLAGS");
    }

    /* Avoid lame compile errors in JSRope::flatten */
    friend class JSRope;

  protected:
    template <typename CharT>
    MOZ_ALWAYS_INLINE
    void setNonInlineChars(const CharT* chars);

  public:
    /* All strings have length. */

    MOZ_ALWAYS_INLINE
    size_t length() const {
        return d.u1.length;
    }

    MOZ_ALWAYS_INLINE
    bool empty() const {
        return d.u1.length == 0;
    }

    inline bool getChar(js::ExclusiveContext* cx, size_t index, char16_t* code);

    /* Strings have either Latin1 or TwoByte chars. */
    bool hasLatin1Chars() const {
        return d.u1.flags & LATIN1_CHARS_BIT;
    }
    bool hasTwoByteChars() const {
        return !(d.u1.flags & LATIN1_CHARS_BIT);
    }

    /* Fallible conversions to more-derived string types. */

    inline JSLinearString* ensureLinear(js::ExclusiveContext* cx);
    inline JSFlatString* ensureFlat(js::ExclusiveContext* cx);

    static bool ensureLinear(js::ExclusiveContext* cx, JSString* str) {
        return str->ensureLinear(cx) != nullptr;
    }

    /* Type query and debug-checked casts */

    MOZ_ALWAYS_INLINE
    bool isRope() const {
        return (d.u1.flags & TYPE_FLAGS_MASK) == ROPE_FLAGS;
    }

    MOZ_ALWAYS_INLINE
    JSRope& asRope() const {
        MOZ_ASSERT(isRope());
        return *(JSRope*)this;
    }

    MOZ_ALWAYS_INLINE
    bool isLinear() const {
        return !isRope();
    }

    MOZ_ALWAYS_INLINE
    JSLinearString& asLinear() const {
        MOZ_ASSERT(JSString::isLinear());
        return *(JSLinearString*)this;
    }

    MOZ_ALWAYS_INLINE
    bool isDependent() const {
        return (d.u1.flags & TYPE_FLAGS_MASK) == DEPENDENT_FLAGS;
    }

    MOZ_ALWAYS_INLINE
    JSDependentString& asDependent() const {
        MOZ_ASSERT(isDependent());
        return *(JSDependentString*)this;
    }

    MOZ_ALWAYS_INLINE
    bool isFlat() const {
        return d.u1.flags & FLAT_BIT;
    }

    MOZ_ALWAYS_INLINE
    JSFlatString& asFlat() const {
        MOZ_ASSERT(isFlat());
        return *(JSFlatString*)this;
    }

    MOZ_ALWAYS_INLINE
    bool isExtensible() const {
        return (d.u1.flags & TYPE_FLAGS_MASK) == EXTENSIBLE_FLAGS;
    }

    MOZ_ALWAYS_INLINE
    JSExtensibleString& asExtensible() const {
        MOZ_ASSERT(isExtensible());
        return *(JSExtensibleString*)this;
    }

    MOZ_ALWAYS_INLINE
    bool isInline() const {
        return d.u1.flags & INLINE_CHARS_BIT;
    }

    MOZ_ALWAYS_INLINE
    JSInlineString& asInline() const {
        MOZ_ASSERT(isInline());
        return *(JSInlineString*)this;
    }

    MOZ_ALWAYS_INLINE
    bool isFatInline() const {
        return (d.u1.flags & FAT_INLINE_MASK) == FAT_INLINE_MASK;
    }

    /* For hot code, prefer other type queries. */
    bool isExternal() const {
        return (d.u1.flags & TYPE_FLAGS_MASK) == EXTERNAL_FLAGS;
    }

    MOZ_ALWAYS_INLINE
    JSExternalString& asExternal() const {
        MOZ_ASSERT(isExternal());
        return *(JSExternalString*)this;
    }

    MOZ_ALWAYS_INLINE
    bool isUndepended() const {
        return (d.u1.flags & TYPE_FLAGS_MASK) == UNDEPENDED_FLAGS;
    }

    MOZ_ALWAYS_INLINE
    bool isAtom() const {
        return d.u1.flags & ATOM_BIT;
    }

    MOZ_ALWAYS_INLINE
    bool isPermanentAtom() const {
        return (d.u1.flags & PERMANENT_ATOM_MASK) == PERMANENT_ATOM_MASK;
    }

    MOZ_ALWAYS_INLINE
    JSAtom& asAtom() const {
        MOZ_ASSERT(isAtom());
        return *(JSAtom*)this;
    }

    /* Only called by the GC for dependent or undepended strings. */

    inline bool hasBase() const {
        return d.u1.flags & HAS_BASE_BIT;
    }

    inline JSLinearString* base() const;

    void traceBase(JSTracer* trc);

    /* Only called by the GC for strings with the AllocKind::STRING kind. */

    inline void finalize(js::FreeOp* fop);

    /* Gets the number of bytes that the chars take on the heap. */

    size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf);

    /* Offsets for direct field from jit code. */

    static size_t offsetOfLength() {
        return offsetof(JSString, d.u1.length);
    }
    static size_t offsetOfFlags() {
        return offsetof(JSString, d.u1.flags);
    }

    static size_t offsetOfNonInlineChars() {
        static_assert(offsetof(JSString, d.s.u2.nonInlineCharsTwoByte) ==
                      offsetof(JSString, d.s.u2.nonInlineCharsLatin1),
                      "nonInlineCharsTwoByte and nonInlineCharsLatin1 must have same offset");
        return offsetof(JSString, d.s.u2.nonInlineCharsTwoByte);
    }

    static inline js::ThingRootKind rootKind() { return js::THING_ROOT_STRING; }

#ifdef DEBUG
    void dump();
    void dumpCharsNoNewline(FILE* fp=stderr);
    void dumpRepresentation(FILE* fp, int indent) const;
    void dumpRepresentationHeader(FILE* fp, int indent, const char* subclass) const;

    template <typename CharT>
    static void dumpChars(const CharT* s, size_t len, FILE* fp=stderr);

    bool equals(const char* s);
#endif

    void traceChildren(JSTracer* trc);

    static MOZ_ALWAYS_INLINE void readBarrier(JSString* thing) {
        if (thing->isPermanentAtom())
            return;

        TenuredCell::readBarrier(thing);
    }

    static MOZ_ALWAYS_INLINE void writeBarrierPre(JSString* thing) {
        if (isNullLike(thing) || thing->isPermanentAtom())
            return;

        TenuredCell::writeBarrierPre(thing);
    }

  private:
    JSString() = delete;
    JSString(const JSString& other) = delete;
    void operator=(const JSString& other) = delete;
};

class JSRope : public JSString
{
    template <typename CharT>
    bool copyCharsInternal(js::ExclusiveContext* cx, js::ScopedJSFreePtr<CharT>& out,
                           bool nullTerminate) const;

    enum UsingBarrier { WithIncrementalBarrier, NoBarrier };

    template<UsingBarrier b, typename CharT>
    JSFlatString* flattenInternal(js::ExclusiveContext* cx);

    template<UsingBarrier b>
    JSFlatString* flattenInternal(js::ExclusiveContext* cx);

    friend class JSString;
    JSFlatString* flatten(js::ExclusiveContext* cx);

    void init(js::ExclusiveContext* cx, JSString* left, JSString* right, size_t length);

  public:
    template <js::AllowGC allowGC>
    static inline JSRope* new_(js::ExclusiveContext* cx,
                               typename js::MaybeRooted<JSString*, allowGC>::HandleType left,
                               typename js::MaybeRooted<JSString*, allowGC>::HandleType right,
                               size_t length);

    bool copyLatin1Chars(js::ExclusiveContext* cx,
                         js::ScopedJSFreePtr<JS::Latin1Char>& out) const;
    bool copyTwoByteChars(js::ExclusiveContext* cx, js::ScopedJSFreePtr<char16_t>& out) const;

    bool copyLatin1CharsZ(js::ExclusiveContext* cx,
                          js::ScopedJSFreePtr<JS::Latin1Char>& out) const;
    bool copyTwoByteCharsZ(js::ExclusiveContext* cx, js::ScopedJSFreePtr<char16_t>& out) const;

    template <typename CharT>
    bool copyChars(js::ExclusiveContext* cx, js::ScopedJSFreePtr<CharT>& out) const;

    JSString* leftChild() const {
        MOZ_ASSERT(isRope());
        return d.s.u2.left;
    }

    JSString* rightChild() const {
        MOZ_ASSERT(isRope());
        return d.s.u3.right;
    }

    void traceChildren(JSTracer* trc);

    static size_t offsetOfLeft() {
        return offsetof(JSRope, d.s.u2.left);
    }
    static size_t offsetOfRight() {
        return offsetof(JSRope, d.s.u3.right);
    }

#ifdef DEBUG
    void dumpRepresentation(FILE* fp, int indent) const;
#endif
};

static_assert(sizeof(JSRope) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

class JSLinearString : public JSString
{
    friend class JSString;
    friend class js::AutoStableStringChars;

    /* Vacuous and therefore unimplemented. */
    JSLinearString* ensureLinear(JSContext* cx) = delete;
    bool isLinear() const = delete;
    JSLinearString& asLinear() const = delete;

  protected:
    /* Returns void pointer to latin1/twoByte chars, for finalizers. */
    MOZ_ALWAYS_INLINE
    void* nonInlineCharsRaw() const {
        MOZ_ASSERT(!isInline());
        static_assert(offsetof(JSLinearString, d.s.u2.nonInlineCharsTwoByte) ==
                      offsetof(JSLinearString, d.s.u2.nonInlineCharsLatin1),
                      "nonInlineCharsTwoByte and nonInlineCharsLatin1 must have same offset");
        return (void*)d.s.u2.nonInlineCharsTwoByte;
    }

    MOZ_ALWAYS_INLINE const JS::Latin1Char* rawLatin1Chars() const;
    MOZ_ALWAYS_INLINE const char16_t* rawTwoByteChars() const;

  public:
    template<typename CharT>
    MOZ_ALWAYS_INLINE
    const CharT* nonInlineChars(const JS::AutoCheckCannotGC& nogc) const;

    MOZ_ALWAYS_INLINE
    const JS::Latin1Char* nonInlineLatin1Chars(const JS::AutoCheckCannotGC& nogc) const {
        MOZ_ASSERT(!isInline());
        MOZ_ASSERT(hasLatin1Chars());
        return d.s.u2.nonInlineCharsLatin1;
    }

    MOZ_ALWAYS_INLINE
    const char16_t* nonInlineTwoByteChars(const JS::AutoCheckCannotGC& nogc) const {
        MOZ_ASSERT(!isInline());
        MOZ_ASSERT(hasTwoByteChars());
        return d.s.u2.nonInlineCharsTwoByte;
    }

    template<typename CharT>
    MOZ_ALWAYS_INLINE
    const CharT* chars(const JS::AutoCheckCannotGC& nogc) const;

    MOZ_ALWAYS_INLINE
    const JS::Latin1Char* latin1Chars(const JS::AutoCheckCannotGC& nogc) const {
        return rawLatin1Chars();
    }

    MOZ_ALWAYS_INLINE
    const char16_t* twoByteChars(const JS::AutoCheckCannotGC& nogc) const {
        return rawTwoByteChars();
    }

    mozilla::Range<const JS::Latin1Char> latin1Range(const JS::AutoCheckCannotGC& nogc) const {
        MOZ_ASSERT(JSString::isLinear());
        return mozilla::Range<const JS::Latin1Char>(latin1Chars(nogc), length());
    }

    mozilla::Range<const char16_t> twoByteRange(const JS::AutoCheckCannotGC& nogc) const {
        MOZ_ASSERT(JSString::isLinear());
        return mozilla::Range<const char16_t>(twoByteChars(nogc), length());
    }

    MOZ_ALWAYS_INLINE
    char16_t latin1OrTwoByteChar(size_t index) const {
        MOZ_ASSERT(JSString::isLinear());
        MOZ_ASSERT(index < length());
        JS::AutoCheckCannotGC nogc;
        return hasLatin1Chars() ? latin1Chars(nogc)[index] : twoByteChars(nogc)[index];
    }

#ifdef DEBUG
    void dumpRepresentationChars(FILE* fp, int indent) const;
#endif
};

static_assert(sizeof(JSLinearString) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

class JSDependentString : public JSLinearString
{
    friend class JSString;
    JSFlatString* undepend(js::ExclusiveContext* cx);

    template <typename CharT>
    JSFlatString* undependInternal(js::ExclusiveContext* cx);

    void init(js::ExclusiveContext* cx, JSLinearString* base, size_t start,
              size_t length);

    /* Vacuous and therefore unimplemented. */
    bool isDependent() const = delete;
    JSDependentString& asDependent() const = delete;

    /* The offset of this string's chars in base->chars(). */
    size_t baseOffset() const {
        MOZ_ASSERT(JSString::isDependent());
        JS::AutoCheckCannotGC nogc;
        size_t offset;
        if (hasTwoByteChars())
            offset = twoByteChars(nogc) - base()->twoByteChars(nogc);
        else
            offset = latin1Chars(nogc) - base()->latin1Chars(nogc);
        MOZ_ASSERT(offset < base()->length());
        return offset;
    }

  public:
    static inline JSLinearString* new_(js::ExclusiveContext* cx, JSLinearString* base,
                                       size_t start, size_t length);

    inline static size_t offsetOfBase() {
        return offsetof(JSDependentString, d.s.u3.base);
    }

#ifdef DEBUG
    void dumpRepresentation(FILE* fp, int indent) const;
#endif
};

static_assert(sizeof(JSDependentString) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

class JSFlatString : public JSLinearString
{
    /* Vacuous and therefore unimplemented. */
    JSFlatString* ensureFlat(JSContext* cx) = delete;
    bool isFlat() const = delete;
    JSFlatString& asFlat() const = delete;

    template <typename CharT>
    static bool isIndexSlow(const CharT* s, size_t length, uint32_t* indexp);

    void init(const char16_t* chars, size_t length);
    void init(const JS::Latin1Char* chars, size_t length);

  public:
    template <js::AllowGC allowGC, typename CharT>
    static inline JSFlatString* new_(js::ExclusiveContext* cx,
                                     const CharT* chars, size_t length);

    /*
     * Returns true if this string's characters store an unsigned 32-bit
     * integer value, initializing *indexp to that value if so.  (Thus if
     * calling isIndex returns true, js::IndexToString(cx, *indexp) will be a
     * string equal to this string.)
     */
    inline bool isIndex(uint32_t* indexp) const {
        MOZ_ASSERT(JSString::isFlat());
        JS::AutoCheckCannotGC nogc;
        if (hasLatin1Chars()) {
            const JS::Latin1Char* s = latin1Chars(nogc);
            return JS7_ISDEC(*s) && isIndexSlow(s, length(), indexp);
        }
        const char16_t* s = twoByteChars(nogc);
        return JS7_ISDEC(*s) && isIndexSlow(s, length(), indexp);
    }

    /*
     * Returns a property name represented by this string, or null on failure.
     * You must verify that this is not an index per isIndex before calling
     * this method.
     */
    inline js::PropertyName* toPropertyName(JSContext* cx);

    /*
     * Once a JSFlatString sub-class has been added to the atom state, this
     * operation changes the string to the JSAtom type, in place.
     */
    MOZ_ALWAYS_INLINE JSAtom* morphAtomizedStringIntoAtom() {
        d.u1.flags |= ATOM_BIT;
        return &asAtom();
    }
    MOZ_ALWAYS_INLINE JSAtom* morphAtomizedStringIntoPermanentAtom() {
        d.u1.flags |= PERMANENT_ATOM_MASK;
        return &asAtom();
    }

    inline void finalize(js::FreeOp* fop);

#ifdef DEBUG
    void dumpRepresentation(FILE* fp, int indent) const;
#endif
};

static_assert(sizeof(JSFlatString) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

class JSExtensibleString : public JSFlatString
{
    /* Vacuous and therefore unimplemented. */
    bool isExtensible() const = delete;
    JSExtensibleString& asExtensible() const = delete;

  public:
    MOZ_ALWAYS_INLINE
    size_t capacity() const {
        MOZ_ASSERT(JSString::isExtensible());
        return d.s.u3.capacity;
    }

#ifdef DEBUG
    void dumpRepresentation(FILE* fp, int indent) const;
#endif
};

static_assert(sizeof(JSExtensibleString) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

class JSInlineString : public JSFlatString
{
  public:
    MOZ_ALWAYS_INLINE
    const JS::Latin1Char* latin1Chars(const JS::AutoCheckCannotGC& nogc) const {
        MOZ_ASSERT(JSString::isInline());
        MOZ_ASSERT(hasLatin1Chars());
        return d.inlineStorageLatin1;
    }

    MOZ_ALWAYS_INLINE
    const char16_t* twoByteChars(const JS::AutoCheckCannotGC& nogc) const {
        MOZ_ASSERT(JSString::isInline());
        MOZ_ASSERT(hasTwoByteChars());
        return d.inlineStorageTwoByte;
    }

    template<typename CharT>
    static bool lengthFits(size_t length);

    static size_t offsetOfInlineStorage() {
        return offsetof(JSInlineString, d.inlineStorageTwoByte);
    }

#ifdef DEBUG
    void dumpRepresentation(FILE* fp, int indent) const;
#endif
};

static_assert(sizeof(JSInlineString) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

/*
 * On 32-bit platforms, JSThinInlineString can store 7 Latin1 characters or 3
 * TwoByte characters (excluding null terminator) inline. On 64-bit platforms,
 * these numbers are 15 and 7, respectively.
 */
class JSThinInlineString : public JSInlineString
{
  public:
    static const size_t MAX_LENGTH_LATIN1 = NUM_INLINE_CHARS_LATIN1 - 1;
    static const size_t MAX_LENGTH_TWO_BYTE = NUM_INLINE_CHARS_TWO_BYTE - 1;

    template <js::AllowGC allowGC>
    static inline JSThinInlineString* new_(js::ExclusiveContext* cx);

    template <typename CharT>
    inline CharT* init(size_t length);

    template<typename CharT>
    static bool lengthFits(size_t length);
};

static_assert(sizeof(JSThinInlineString) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

/*
 * On both 32-bit and 64-bit platforms, MAX_LENGTH_TWO_BYTE is 11 and
 * MAX_LENGTH_LATIN1 is 23 (excluding null terminator). This is deliberate,
 * in order to minimize potential performance differences between 32-bit and
 * 64-bit platforms.
 *
 * There are still some differences due to NUM_INLINE_CHARS_* being different.
 * E.g. TwoByte strings of length 4--7 will be JSFatInlineStrings on 32-bit
 * platforms and JSThinInlineStrings on 64-bit platforms. But the more
 * significant transition from inline strings to non-inline strings occurs at
 * length 11 (for TwoByte strings) and 23 (Latin1 strings) on both 32-bit and
 * 64-bit platforms.
 */
class JSFatInlineString : public JSInlineString
{
    static const size_t INLINE_EXTENSION_CHARS_LATIN1 = 24 - NUM_INLINE_CHARS_LATIN1;
    static const size_t INLINE_EXTENSION_CHARS_TWO_BYTE = 12 - NUM_INLINE_CHARS_TWO_BYTE;

  protected: /* to fool clang into not warning this is unused */
    union {
        char   inlineStorageExtensionLatin1[INLINE_EXTENSION_CHARS_LATIN1];
        char16_t inlineStorageExtensionTwoByte[INLINE_EXTENSION_CHARS_TWO_BYTE];
    };

  public:
    template <js::AllowGC allowGC>
    static inline JSFatInlineString* new_(js::ExclusiveContext* cx);

    static const size_t MAX_LENGTH_LATIN1 = JSString::NUM_INLINE_CHARS_LATIN1 +
                                            INLINE_EXTENSION_CHARS_LATIN1
                                            -1 /* null terminator */;

    static const size_t MAX_LENGTH_TWO_BYTE = JSString::NUM_INLINE_CHARS_TWO_BYTE +
                                              INLINE_EXTENSION_CHARS_TWO_BYTE
                                              -1 /* null terminator */;

    template <typename CharT>
    inline CharT* init(size_t length);

    template<typename CharT>
    static bool lengthFits(size_t length);

    /* Only called by the GC for strings with the AllocKind::FAT_INLINE_STRING kind. */

    MOZ_ALWAYS_INLINE void finalize(js::FreeOp* fop);
};

static_assert(sizeof(JSFatInlineString) % js::gc::CellSize == 0,
              "fat inline strings shouldn't waste space up to the next cell "
              "boundary");

class JSExternalString : public JSFlatString
{
    void init(const char16_t* chars, size_t length, const JSStringFinalizer* fin);

    /* Vacuous and therefore unimplemented. */
    bool isExternal() const = delete;
    JSExternalString& asExternal() const = delete;

  public:
    static inline JSExternalString* new_(JSContext* cx, const char16_t* chars, size_t length,
                                         const JSStringFinalizer* fin);

    const JSStringFinalizer* externalFinalizer() const {
        MOZ_ASSERT(JSString::isExternal());
        return d.s.u3.externalFinalizer;
    }

    /*
     * External chars are never allocated inline or in the nursery, so we can
     * safely expose this without requiring an AutoCheckCannotGC argument.
     */
    const char16_t* twoByteChars() const {
        return rawTwoByteChars();
    }

    /* Only called by the GC for strings with the AllocKind::EXTERNAL_STRING kind. */

    inline void finalize(js::FreeOp* fop);

#ifdef DEBUG
    void dumpRepresentation(FILE* fp, int indent) const;
#endif
};

static_assert(sizeof(JSExternalString) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

class JSUndependedString : public JSFlatString
{
    /*
     * JSUndependedString is not explicitly used and is only present for
     * consistency. See JSDependentString::undepend for how a JSDependentString
     * gets morphed into a JSUndependedString.
     */
};

static_assert(sizeof(JSUndependedString) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

class JSAtom : public JSFlatString
{
    /* Vacuous and therefore unimplemented. */
    bool isAtom() const = delete;
    JSAtom& asAtom() const = delete;

  public:
    /* Returns the PropertyName for this.  isIndex() must be false. */
    inline js::PropertyName* asPropertyName();

    inline void finalize(js::FreeOp* fop);

    MOZ_ALWAYS_INLINE
    bool isPermanent() const {
        return JSString::isPermanentAtom();
    }

    // Transform this atom into a permanent atom. This is only done during
    // initialization of the runtime.
    MOZ_ALWAYS_INLINE void morphIntoPermanentAtom() {
        d.u1.flags |= PERMANENT_ATOM_MASK;
    }

#ifdef DEBUG
    void dump();
#endif
};

static_assert(sizeof(JSAtom) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

namespace js {

class StaticStrings
{
  private:
    /* Bigger chars cannot be in a length-2 string. */
    static const size_t SMALL_CHAR_LIMIT    = 128U;
    static const size_t NUM_SMALL_CHARS     = 64U;

    JSAtom* length2StaticTable[NUM_SMALL_CHARS * NUM_SMALL_CHARS];

  public:
    /* We keep these public for the JITs. */
    static const size_t UNIT_STATIC_LIMIT   = 256U;
    JSAtom* unitStaticTable[UNIT_STATIC_LIMIT];

    static const size_t INT_STATIC_LIMIT    = 256U;
    JSAtom* intStaticTable[INT_STATIC_LIMIT];

    StaticStrings() {
        mozilla::PodZero(this);
    }

    bool init(JSContext* cx);
    void trace(JSTracer* trc);

    static bool hasUint(uint32_t u) { return u < INT_STATIC_LIMIT; }

    JSAtom* getUint(uint32_t u) {
        MOZ_ASSERT(hasUint(u));
        return intStaticTable[u];
    }

    static bool hasInt(int32_t i) {
        return uint32_t(i) < INT_STATIC_LIMIT;
    }

    JSAtom* getInt(int32_t i) {
        MOZ_ASSERT(hasInt(i));
        return getUint(uint32_t(i));
    }

    static bool hasUnit(char16_t c) { return c < UNIT_STATIC_LIMIT; }

    JSAtom* getUnit(char16_t c) {
        MOZ_ASSERT(hasUnit(c));
        return unitStaticTable[c];
    }

    /* May not return atom, returns null on (reported) failure. */
    inline JSLinearString* getUnitStringForElement(JSContext* cx, JSString* str, size_t index);

    template <typename CharT>
    static bool isStatic(const CharT* chars, size_t len);
    static bool isStatic(JSAtom* atom);

    /* Return null if no static atom exists for the given (chars, length). */
    template <typename CharT>
    JSAtom* lookup(const CharT* chars, size_t length) {
        switch (length) {
          case 1: {
            char16_t c = chars[0];
            if (c < UNIT_STATIC_LIMIT)
                return getUnit(c);
            return nullptr;
          }
          case 2:
            if (fitsInSmallChar(chars[0]) && fitsInSmallChar(chars[1]))
                return getLength2(chars[0], chars[1]);
            return nullptr;
          case 3:
            /*
             * Here we know that JSString::intStringTable covers only 256 (or at least
             * not 1000 or more) chars. We rely on order here to resolve the unit vs.
             * int string/length-2 string atom identity issue by giving priority to unit
             * strings for "0" through "9" and length-2 strings for "10" through "99".
             */
            static_assert(INT_STATIC_LIMIT <= 999,
                          "static int strings assumed below to be at most "
                          "three digits");
            if ('1' <= chars[0] && chars[0] <= '9' &&
                '0' <= chars[1] && chars[1] <= '9' &&
                '0' <= chars[2] && chars[2] <= '9') {
                int i = (chars[0] - '0') * 100 +
                          (chars[1] - '0') * 10 +
                          (chars[2] - '0');

                if (unsigned(i) < INT_STATIC_LIMIT)
                    return getInt(i);
            }
            return nullptr;
        }

        return nullptr;
    }

  private:
    typedef uint8_t SmallChar;
    static const SmallChar INVALID_SMALL_CHAR = -1;

    static bool fitsInSmallChar(char16_t c) {
        return c < SMALL_CHAR_LIMIT && toSmallChar[c] != INVALID_SMALL_CHAR;
    }

    static const SmallChar toSmallChar[];

    JSAtom* getLength2(char16_t c1, char16_t c2);
    JSAtom* getLength2(uint32_t u) {
        MOZ_ASSERT(u < 100);
        return getLength2('0' + u / 10, '0' + u % 10);
    }
};

/*
 * Represents an atomized string which does not contain an index (that is, an
 * unsigned 32-bit value).  Thus for any PropertyName propname,
 * ToString(ToUint32(propname)) never equals propname.
 *
 * To more concretely illustrate the utility of PropertyName, consider that it
 * is used to partition, in a type-safe manner, the ways to refer to a
 * property, as follows:
 *
 *   - uint32_t indexes,
 *   - PropertyName strings which don't encode uint32_t indexes, and
 *   - jsspecial special properties (non-ES5 properties like object-valued
 *     jsids, JSID_EMPTY, JSID_VOID, and maybe in the future Harmony-proposed
 *     private names).
 */
class PropertyName : public JSAtom
{
  private:
    /* Vacuous and therefore unimplemented. */
    PropertyName* asPropertyName() = delete;
};

static_assert(sizeof(PropertyName) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

static MOZ_ALWAYS_INLINE jsid
NameToId(PropertyName* name)
{
    return NON_INTEGER_ATOM_TO_JSID(name);
}

using PropertyNameVector = js::TraceableVector<PropertyName*>;

template <typename CharT>
void
CopyChars(CharT* dest, const JSLinearString& str);

/* GC-allocate a string descriptor for the given malloc-allocated chars. */
template <js::AllowGC allowGC, typename CharT>
extern JSFlatString*
NewString(js::ExclusiveContext* cx, CharT* chars, size_t length);

/* Like NewString, but doesn't try to deflate to Latin1. */
template <js::AllowGC allowGC, typename CharT>
extern JSFlatString*
NewStringDontDeflate(js::ExclusiveContext* cx, CharT* chars, size_t length);

extern JSLinearString*
NewDependentString(JSContext* cx, JSString* base, size_t start, size_t length);

/* Copy a counted string and GC-allocate a descriptor for it. */
template <js::AllowGC allowGC, typename CharT>
extern JSFlatString*
NewStringCopyN(js::ExclusiveContext* cx, const CharT* s, size_t n);

template <js::AllowGC allowGC>
inline JSFlatString*
NewStringCopyN(ExclusiveContext* cx, const char* s, size_t n)
{
    return NewStringCopyN<allowGC>(cx, reinterpret_cast<const Latin1Char*>(s), n);
}

/* Like NewStringCopyN, but doesn't try to deflate to Latin1. */
template <js::AllowGC allowGC, typename CharT>
extern JSFlatString*
NewStringCopyNDontDeflate(js::ExclusiveContext* cx, const CharT* s, size_t n);

/* Copy a C string and GC-allocate a descriptor for it. */
template <js::AllowGC allowGC>
inline JSFlatString*
NewStringCopyZ(js::ExclusiveContext* cx, const char16_t* s)
{
    return NewStringCopyN<allowGC>(cx, s, js_strlen(s));
}

template <js::AllowGC allowGC>
inline JSFlatString*
NewStringCopyZ(js::ExclusiveContext* cx, const char* s)
{
    return NewStringCopyN<allowGC>(cx, s, strlen(s));
}

} /* namespace js */

// Addon IDs are interned atoms which are never destroyed. This detail is
// not exposed outside the API.
class JSAddonId : public JSAtom
{};

MOZ_ALWAYS_INLINE bool
JSString::getChar(js::ExclusiveContext* cx, size_t index, char16_t* code)
{
    MOZ_ASSERT(index < length());

    /*
     * Optimization for one level deep ropes.
     * This is common for the following pattern:
     *
     * while() {
     *   text = text.substr(0, x) + "bla" + text.substr(x)
     *   test.charCodeAt(x + 1)
     * }
     */
    JSString* str;
    if (isRope()) {
        JSRope* rope = &asRope();
        if (uint32_t(index) < rope->leftChild()->length()) {
            str = rope->leftChild();
        } else {
            str = rope->rightChild();
            index -= rope->leftChild()->length();
        }
    } else {
        str = this;
    }

    if (!str->ensureLinear(cx))
        return false;

    *code = str->asLinear().latin1OrTwoByteChar(index);
    return true;
}

MOZ_ALWAYS_INLINE JSLinearString*
JSString::ensureLinear(js::ExclusiveContext* cx)
{
    return isLinear()
           ? &asLinear()
           : asRope().flatten(cx);
}

MOZ_ALWAYS_INLINE JSFlatString*
JSString::ensureFlat(js::ExclusiveContext* cx)
{
    return isFlat()
           ? &asFlat()
           : isDependent()
             ? asDependent().undepend(cx)
             : asRope().flatten(cx);
}

inline JSLinearString*
JSString::base() const
{
    MOZ_ASSERT(hasBase());
    MOZ_ASSERT(!d.s.u3.base->isInline());
    return d.s.u3.base;
}

template<>
MOZ_ALWAYS_INLINE const char16_t*
JSLinearString::nonInlineChars(const JS::AutoCheckCannotGC& nogc) const
{
    return nonInlineTwoByteChars(nogc);
}

template<>
MOZ_ALWAYS_INLINE const JS::Latin1Char*
JSLinearString::nonInlineChars(const JS::AutoCheckCannotGC& nogc) const
{
    return nonInlineLatin1Chars(nogc);
}

template<>
MOZ_ALWAYS_INLINE const char16_t*
JSLinearString::chars(const JS::AutoCheckCannotGC& nogc) const
{
    return rawTwoByteChars();
}

template<>
MOZ_ALWAYS_INLINE const JS::Latin1Char*
JSLinearString::chars(const JS::AutoCheckCannotGC& nogc) const
{
    return rawLatin1Chars();
}

template <>
MOZ_ALWAYS_INLINE bool
JSRope::copyChars<JS::Latin1Char>(js::ExclusiveContext* cx,
                                  js::ScopedJSFreePtr<JS::Latin1Char>& out) const
{
    return copyLatin1Chars(cx, out);
}

template <>
MOZ_ALWAYS_INLINE bool
JSRope::copyChars<char16_t>(js::ExclusiveContext* cx, js::ScopedJSFreePtr<char16_t>& out) const
{
    return copyTwoByteChars(cx, out);
}

template<>
MOZ_ALWAYS_INLINE bool
JSThinInlineString::lengthFits<JS::Latin1Char>(size_t length)
{
    return length <= MAX_LENGTH_LATIN1;
}

template<>
MOZ_ALWAYS_INLINE bool
JSThinInlineString::lengthFits<char16_t>(size_t length)
{
    return length <= MAX_LENGTH_TWO_BYTE;
}

template<>
MOZ_ALWAYS_INLINE bool
JSFatInlineString::lengthFits<JS::Latin1Char>(size_t length)
{
    static_assert((INLINE_EXTENSION_CHARS_LATIN1 * sizeof(char)) % js::gc::CellSize == 0,
                  "fat inline strings' Latin1 characters don't exactly "
                  "fill subsequent cells and thus are wasteful");
    static_assert(MAX_LENGTH_LATIN1 + 1 ==
                  (sizeof(JSFatInlineString) -
                   offsetof(JSFatInlineString, d.inlineStorageLatin1)) / sizeof(char),
                  "MAX_LENGTH_LATIN1 must be one less than inline Latin1 "
                  "storage count");

    return length <= MAX_LENGTH_LATIN1;
}

template<>
MOZ_ALWAYS_INLINE bool
JSFatInlineString::lengthFits<char16_t>(size_t length)
{
    static_assert((INLINE_EXTENSION_CHARS_TWO_BYTE * sizeof(char16_t)) % js::gc::CellSize == 0,
                  "fat inline strings' char16_t characters don't exactly "
                  "fill subsequent cells and thus are wasteful");
    static_assert(MAX_LENGTH_TWO_BYTE + 1 ==
                  (sizeof(JSFatInlineString) -
                   offsetof(JSFatInlineString, d.inlineStorageTwoByte)) / sizeof(char16_t),
                  "MAX_LENGTH_TWO_BYTE must be one less than inline "
                  "char16_t storage count");

    return length <= MAX_LENGTH_TWO_BYTE;
}

template<>
MOZ_ALWAYS_INLINE bool
JSInlineString::lengthFits<JS::Latin1Char>(size_t length)
{
    // If it fits in a fat inline string, it fits in any inline string.
    return JSFatInlineString::lengthFits<JS::Latin1Char>(length);
}

template<>
MOZ_ALWAYS_INLINE bool
JSInlineString::lengthFits<char16_t>(size_t length)
{
    // If it fits in a fat inline string, it fits in any inline string.
    return JSFatInlineString::lengthFits<char16_t>(length);
}

template<>
MOZ_ALWAYS_INLINE void
JSString::setNonInlineChars(const char16_t* chars)
{
    d.s.u2.nonInlineCharsTwoByte = chars;
}

template<>
MOZ_ALWAYS_INLINE void
JSString::setNonInlineChars(const JS::Latin1Char* chars)
{
    d.s.u2.nonInlineCharsLatin1 = chars;
}

MOZ_ALWAYS_INLINE const JS::Latin1Char*
JSLinearString::rawLatin1Chars() const
{
    MOZ_ASSERT(JSString::isLinear());
    MOZ_ASSERT(hasLatin1Chars());
    return isInline() ? d.inlineStorageLatin1 : d.s.u2.nonInlineCharsLatin1;
}

MOZ_ALWAYS_INLINE const char16_t*
JSLinearString::rawTwoByteChars() const
{
    MOZ_ASSERT(JSString::isLinear());
    MOZ_ASSERT(hasTwoByteChars());
    return isInline() ? d.inlineStorageTwoByte : d.s.u2.nonInlineCharsTwoByte;
}

inline js::PropertyName*
JSAtom::asPropertyName()
{
#ifdef DEBUG
    uint32_t dummy;
    MOZ_ASSERT(!isIndex(&dummy));
#endif
    return static_cast<js::PropertyName*>(this);
}

#endif /* vm_String_h */
