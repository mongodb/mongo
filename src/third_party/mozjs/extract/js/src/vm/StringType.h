/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_StringType_h
#define vm_StringType_h

#include "mozilla/Maybe.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/Range.h"
#include "mozilla/TextUtils.h"

#include <type_traits>  // std::is_same

#include "jsapi.h"
#include "jstypes.h"  // js::Bit

#include "gc/Allocator.h"
#include "gc/Barrier.h"
#include "gc/Cell.h"
#include "gc/MaybeRooted.h"
#include "gc/Nursery.h"
#include "gc/RelocationOverlay.h"
#include "gc/Rooting.h"
#include "js/CharacterEncoding.h"
#include "js/RootingAPI.h"
#include "js/shadow/String.h"  // JS::shadow::String
#include "js/String.h"         // JS::MaxStringLength
#include "js/UniquePtr.h"
#include "util/Text.h"
#include "vm/Printer.h"

class JSDependentString;
class JSExtensibleString;
class JSExternalString;
class JSInlineString;
class JSRope;

namespace JS {

class JS_PUBLIC_API AutoStableStringChars;

}  // namespace JS

namespace js {

namespace frontend {

class ParserAtomsTable;
class TaggedParserAtomIndex;
class WellKnownParserAtoms;
struct CompilationAtomCache;

}  // namespace frontend

class StaticStrings;
class PropertyName;

/* The buffer length required to contain any unsigned 32-bit integer. */
static const size_t UINT32_CHAR_BUFFER_LENGTH = sizeof("4294967295") - 1;

// Maximum array index. This value is defined in the spec (ES2021 draft, 6.1.7):
//
//   An array index is an integer index whose numeric value i is in the range
//   +0𝔽 ≤ i < 𝔽(2^32 - 1).
const uint32_t MAX_ARRAY_INDEX = 4294967294u;  // 2^32-2 (= UINT32_MAX-1)

// Returns true if the characters of `s` store an unsigned 32-bit integer value
// less than or equal to MAX_ARRAY_INDEX, initializing `*indexp` to that value
// if so. Leading '0' isn't allowed except 0 itself.
template <typename CharT>
bool CheckStringIsIndex(const CharT* s, size_t length, uint32_t* indexp);

} /* namespace js */

// clang-format off
/*
 * [SMDOC] JavaScript Strings
 *
 * Conceptually, a JS string is just an array of chars and a length. This array
 * of chars may or may not be null-terminated and, if it is, the null character
 * is not included in the length.
 *
 * To improve performance of common operations, the following optimizations are
 * made which affect the engine's representation of strings:
 *
 *  - The plain vanilla representation is a "linear" string which consists of a
 *    string header in the GC heap and a malloc'd char array.
 *
 *  - To avoid copying a substring of an existing "base" string , a "dependent"
 *    string (JSDependentString) can be created which points into the base
 *    string's char array.
 *
 *  - To avoid O(n^2) char buffer copying, a "rope" node (JSRope) can be created
 *    to represent a delayed string concatenation. Concatenation (called
 *    flattening) is performed if and when a linear char array is requested. In
 *    general, ropes form a binary dag whose internal nodes are JSRope string
 *    headers with no associated char array and whose leaf nodes are linear
 *    strings.
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
 *    JSAPI hands out pointers to linear strings' buffers, so resizing with
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
 *  - To avoid using two bytes per character for every string, string
 *    characters are stored as Latin1 instead of TwoByte if all characters are
 *    representable in Latin1.
 *
 *  - To avoid slow conversions from strings to integer indexes, we cache 16 bit
 *    unsigned indexes on strings representing such numbers.
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
 * JSLinearString (abstract)    latin1Chars, twoByteChars / -
 *  |
 *  +-- JSDependentString       base / -
 *  |
 *  +-- JSExternalString        - / char array memory managed by embedding
 *  |
 *  +-- JSExtensibleString      tracks total buffer capacity (including current text)
 *  |
 *  +-- JSInlineString (abstract) - / chars stored in header
 *  |   |
 *  |   +-- JSThinInlineString  - / header is normal
 *  |   |
 *  |   +-- JSFatInlineString   - / header is fat
 *  |
 * JSAtom (abstract)            - / string equality === pointer equality
 *  |  |
 *  |  +-- js::NormalAtom       - JSLinearString + atom hash code
 *  |  |
 *  |  +-- js::FatInlineAtom    - JSFatInlineString + atom hash code
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
 * most-derived types. Furthermore, each atom stores a hash number (based on its
 * chars). This hash number is used as key in the atoms table and when the atom
 * is used as key in a JS Map/Set.
 *
 * Derived string types can be queried from ancestor types via isX() and
 * retrieved with asX() debug-only-checked casts.
 *
 * The ensureX() operations mutate 'this' in place to effectively the type to be
 * at least X (e.g., ensureLinear will change a JSRope to be a JSLinearString).
 */
// clang-format on

class JSString : public js::gc::CellWithLengthAndFlags {
 protected:
  static const size_t NUM_INLINE_CHARS_LATIN1 =
      2 * sizeof(void*) / sizeof(JS::Latin1Char);
  static const size_t NUM_INLINE_CHARS_TWO_BYTE =
      2 * sizeof(void*) / sizeof(char16_t);

 public:
  // String length and flags are stored in the cell header.
  MOZ_ALWAYS_INLINE
  size_t length() const { return headerLengthField(); }
  MOZ_ALWAYS_INLINE
  uint32_t flags() const { return headerFlagsField(); }

 protected:
  /* Fields only apply to string types commented on the right. */
  struct Data {
    // Note: 32-bit length and flags fields are inherited from
    // CellWithLengthAndFlags.

    union {
      union {
        /* JS(Fat)InlineString */
        JS::Latin1Char inlineStorageLatin1[NUM_INLINE_CHARS_LATIN1];
        char16_t inlineStorageTwoByte[NUM_INLINE_CHARS_TWO_BYTE];
      };
      struct {
        union {
          const JS::Latin1Char* nonInlineCharsLatin1; /* JSLinearString, except
                                                         JS(Fat)InlineString */
          const char16_t* nonInlineCharsTwoByte;      /* JSLinearString, except
                                                         JS(Fat)InlineString */
          JSString* left;                             /* JSRope */
          JSRope* parent;                             /* Used in flattening */
        } u2;
        union {
          JSLinearString* base; /* JSDependentString */
          JSString* right;      /* JSRope */
          size_t capacity;      /* JSLinearString (extensible) */
          const JSExternalStringCallbacks*
              externalCallbacks; /* JSExternalString */
        } u3;
      } s;
    };
  } d;

 public:
  /* Flags exposed only for jits */

  /*
   * Flag Encoding
   *
   * The first word of a JSString stores flags, index, and (on some
   * platforms) the length. The flags store both the string's type and its
   * character encoding.
   *
   * If LATIN1_CHARS_BIT is set, the string's characters are stored as Latin1
   * instead of TwoByte. This flag can also be set for ropes, if both the
   * left and right nodes are Latin1. Flattening will result in a Latin1
   * string in this case.
   *
   * The other flags store the string's type. Instead of using a dense index
   * to represent the most-derived type, string types are encoded to allow
   * single-op tests for hot queries (isRope, isDependent, isAtom) which, in
   * view of subtyping, would require slower (isX() || isY() || isZ()).
   *
   * The string type encoding can be summarized as follows. The "instance
   * encoding" entry for a type specifies the flag bits used to create a
   * string instance of that type. Abstract types have no instances and thus
   * have no such entry. The "subtype predicate" entry for a type specifies
   * the predicate used to query whether a JSString instance is subtype
   * (reflexively) of that type.
   *
   *   String        Instance         Subtype
   *   type          encoding         predicate
   *   -----------------------------------------
   *   Rope          000000 000       xxxx0x xxx
   *   Linear        -                xxxx1x xxx
   *   Dependent     000110 000       xxx1xx xxx
   *   External      100010 000       100010 xxx
   *   Extensible    010010 000       010010 xxx
   *   Inline        001010 000       xx1xxx xxx
   *   FatInline     011010 000       x11xxx xxx
   *   NormalAtom    000011 000       xxxxx1 xxx
   *   PermanentAtom 100011 000       1xxxx1 xxx
   *   InlineAtom    -                xx1xx1 xxx
   *   FatInlineAtom -                x11xx1 xxx
   *
   * Bits 0..2 are reserved for use by the GC (see
   * gc::CellFlagBitsReservedForGC). In particular, bit 0 is currently used for
   * FORWARD_BIT for forwarded nursery cells. The other 2 bits are currently
   * unused.
   *
   * Note that the first 4 flag bits 3..6 (from right to left in the previous
   * table) have the following meaning and can be used for some hot queries:
   *
   *   Bit 3: IsAtom (Atom, PermanentAtom)
   *   Bit 4: IsLinear
   *   Bit 5: IsDependent
   *   Bit 6: IsInline (Inline, FatInline)
   *
   * If INDEX_VALUE_BIT is set, bits 16 and up will also hold an integer index.
   */

  // The low bits of flag word are reserved by GC.
  static_assert(js::gc::CellFlagBitsReservedForGC <= 3,
                "JSString::flags must reserve enough bits for Cell");

  static const uint32_t ATOM_BIT = js::Bit(3);
  static const uint32_t LINEAR_BIT = js::Bit(4);
  static const uint32_t DEPENDENT_BIT = js::Bit(5);
  static const uint32_t INLINE_CHARS_BIT = js::Bit(6);

  static const uint32_t EXTENSIBLE_FLAGS = LINEAR_BIT | js::Bit(7);
  static const uint32_t EXTERNAL_FLAGS = LINEAR_BIT | js::Bit(8);

  static const uint32_t FAT_INLINE_MASK = INLINE_CHARS_BIT | js::Bit(7);
  static const uint32_t PERMANENT_ATOM_MASK = ATOM_BIT | js::Bit(8);

  /* Initial flags for various types of strings. */
  static const uint32_t INIT_THIN_INLINE_FLAGS = LINEAR_BIT | INLINE_CHARS_BIT;
  static const uint32_t INIT_FAT_INLINE_FLAGS = LINEAR_BIT | FAT_INLINE_MASK;
  static const uint32_t INIT_ROPE_FLAGS = 0;
  static const uint32_t INIT_LINEAR_FLAGS = LINEAR_BIT;
  static const uint32_t INIT_DEPENDENT_FLAGS = LINEAR_BIT | DEPENDENT_BIT;

  static const uint32_t TYPE_FLAGS_MASK = js::BitMask(9) - js::BitMask(3);
  static_assert((TYPE_FLAGS_MASK & RESERVED_MASK) == 0,
                "GC reserved bits must not be used for Strings");

  static const uint32_t LATIN1_CHARS_BIT = js::Bit(9);

  // Whether this atom's characters store an uint32 index value less than or
  // equal to MAX_ARRAY_INDEX. Not used for non-atomized strings.
  // See JSLinearString::isIndex.
  static const uint32_t ATOM_IS_INDEX_BIT = js::Bit(10);

  static const uint32_t INDEX_VALUE_BIT = js::Bit(11);
  static const uint32_t INDEX_VALUE_SHIFT = 16;

  // NON_DEDUP_BIT is used in string deduplication during tenuring.
  static const uint32_t NON_DEDUP_BIT = js::Bit(12);

  // If IN_STRING_TO_ATOM_CACHE is set, this string had an entry in the
  // StringToAtomCache at some point. Note that GC can purge the cache without
  // clearing this bit.
  static const uint32_t IN_STRING_TO_ATOM_CACHE = js::Bit(13);

  // Flags used during rope flattening that indicate what action to perform when
  // returning to the rope's parent rope.
  static const uint32_t FLATTEN_VISIT_RIGHT = js::Bit(14);
  static const uint32_t FLATTEN_FINISH_NODE = js::Bit(15);
  static const uint32_t FLATTEN_MASK =
      FLATTEN_VISIT_RIGHT | FLATTEN_FINISH_NODE;

  static const uint32_t MAX_LENGTH = JS::MaxStringLength;

  static const JS::Latin1Char MAX_LATIN1_CHAR = 0xff;

  /*
   * Helper function to validate that a string of a given length is
   * representable by a JSString. An allocation overflow is reported if false
   * is returned.
   */
  static inline bool validateLength(JSContext* maybecx, size_t length);

  static constexpr size_t offsetOfFlags() { return offsetOfHeaderFlags(); }
  static constexpr size_t offsetOfLength() { return offsetOfHeaderLength(); }

  bool sameLengthAndFlags(const JSString& other) const {
    return length() == other.length() && flags() == other.flags();
  }

  static void staticAsserts() {
    static_assert(JSString::MAX_LENGTH < UINT32_MAX,
                  "Length must fit in 32 bits");
    static_assert(
        sizeof(JSString) == (offsetof(JSString, d.inlineStorageLatin1) +
                             NUM_INLINE_CHARS_LATIN1 * sizeof(char)),
        "Inline Latin1 chars must fit in a JSString");
    static_assert(
        sizeof(JSString) == (offsetof(JSString, d.inlineStorageTwoByte) +
                             NUM_INLINE_CHARS_TWO_BYTE * sizeof(char16_t)),
        "Inline char16_t chars must fit in a JSString");

    /* Ensure js::shadow::String has the same layout. */
    using JS::shadow::String;
    static_assert(
        JSString::offsetOfRawHeaderFlagsField() == offsetof(String, flags_),
        "shadow::String flags offset must match JSString");
#if JS_BITS_PER_WORD == 32
    static_assert(JSString::offsetOfLength() == offsetof(String, length_),
                  "shadow::String length offset must match JSString");
#endif
    static_assert(offsetof(JSString, d.s.u2.nonInlineCharsLatin1) ==
                      offsetof(String, nonInlineCharsLatin1),
                  "shadow::String nonInlineChars offset must match JSString");
    static_assert(offsetof(JSString, d.s.u2.nonInlineCharsTwoByte) ==
                      offsetof(String, nonInlineCharsTwoByte),
                  "shadow::String nonInlineChars offset must match JSString");
    static_assert(
        offsetof(JSString, d.s.u3.externalCallbacks) ==
            offsetof(String, externalCallbacks),
        "shadow::String externalCallbacks offset must match JSString");
    static_assert(offsetof(JSString, d.inlineStorageLatin1) ==
                      offsetof(String, inlineStorageLatin1),
                  "shadow::String inlineStorage offset must match JSString");
    static_assert(offsetof(JSString, d.inlineStorageTwoByte) ==
                      offsetof(String, inlineStorageTwoByte),
                  "shadow::String inlineStorage offset must match JSString");
    static_assert(ATOM_BIT == String::ATOM_BIT,
                  "shadow::String::ATOM_BIT must match JSString::ATOM_BIT");
    static_assert(LINEAR_BIT == String::LINEAR_BIT,
                  "shadow::String::LINEAR_BIT must match JSString::LINEAR_BIT");
    static_assert(INLINE_CHARS_BIT == String::INLINE_CHARS_BIT,
                  "shadow::String::INLINE_CHARS_BIT must match "
                  "JSString::INLINE_CHARS_BIT");
    static_assert(LATIN1_CHARS_BIT == String::LATIN1_CHARS_BIT,
                  "shadow::String::LATIN1_CHARS_BIT must match "
                  "JSString::LATIN1_CHARS_BIT");
    static_assert(
        TYPE_FLAGS_MASK == String::TYPE_FLAGS_MASK,
        "shadow::String::TYPE_FLAGS_MASK must match JSString::TYPE_FLAGS_MASK");
    static_assert(
        EXTERNAL_FLAGS == String::EXTERNAL_FLAGS,
        "shadow::String::EXTERNAL_FLAGS must match JSString::EXTERNAL_FLAGS");
  }

  /* Avoid silly compile errors in JSRope::flatten */
  friend class JSRope;

  friend class js::gc::RelocationOverlay;

 protected:
  template <typename CharT>
  MOZ_ALWAYS_INLINE void setNonInlineChars(const CharT* chars);

  template <typename CharT>
  static MOZ_ALWAYS_INLINE void checkStringCharsArena(const CharT* chars) {
#ifdef MOZ_DEBUG
    js::AssertJSStringBufferInCorrectArena(chars);
#endif
  }

  // Get correct non-inline chars enum arm for given type
  template <typename CharT>
  MOZ_ALWAYS_INLINE const CharT* nonInlineCharsRaw() const;

 public:
  MOZ_ALWAYS_INLINE
  bool empty() const { return length() == 0; }

  inline bool getChar(JSContext* cx, size_t index, char16_t* code);

  /* Strings have either Latin1 or TwoByte chars. */
  bool hasLatin1Chars() const { return flags() & LATIN1_CHARS_BIT; }
  bool hasTwoByteChars() const { return !(flags() & LATIN1_CHARS_BIT); }

  /* Strings might contain cached indexes. */
  bool hasIndexValue() const { return flags() & INDEX_VALUE_BIT; }
  uint32_t getIndexValue() const {
    MOZ_ASSERT(hasIndexValue());
    MOZ_ASSERT(isLinear());
    return flags() >> INDEX_VALUE_SHIFT;
  }

  inline size_t allocSize() const;

  /* Fallible conversions to more-derived string types. */

  inline JSLinearString* ensureLinear(JSContext* cx);

  static bool ensureLinear(JSContext* cx, JSString* str) {
    return str->ensureLinear(cx) != nullptr;
  }

  /* Type query and debug-checked casts */

  MOZ_ALWAYS_INLINE
  bool isRope() const { return !(flags() & LINEAR_BIT); }

  MOZ_ALWAYS_INLINE
  JSRope& asRope() const {
    MOZ_ASSERT(isRope());
    return *(JSRope*)this;
  }

  MOZ_ALWAYS_INLINE
  bool isLinear() const { return flags() & LINEAR_BIT; }

  MOZ_ALWAYS_INLINE
  JSLinearString& asLinear() const {
    MOZ_ASSERT(JSString::isLinear());
    return *(JSLinearString*)this;
  }

  MOZ_ALWAYS_INLINE
  bool isDependent() const { return flags() & DEPENDENT_BIT; }

  MOZ_ALWAYS_INLINE
  JSDependentString& asDependent() const {
    MOZ_ASSERT(isDependent());
    return *(JSDependentString*)this;
  }

  MOZ_ALWAYS_INLINE
  bool isExtensible() const {
    return (flags() & TYPE_FLAGS_MASK) == EXTENSIBLE_FLAGS;
  }

  MOZ_ALWAYS_INLINE
  JSExtensibleString& asExtensible() const {
    MOZ_ASSERT(isExtensible());
    return *(JSExtensibleString*)this;
  }

  MOZ_ALWAYS_INLINE
  bool isInline() const { return flags() & INLINE_CHARS_BIT; }

  MOZ_ALWAYS_INLINE
  JSInlineString& asInline() const {
    MOZ_ASSERT(isInline());
    return *(JSInlineString*)this;
  }

  MOZ_ALWAYS_INLINE
  bool isFatInline() const {
    return (flags() & FAT_INLINE_MASK) == FAT_INLINE_MASK;
  }

  /* For hot code, prefer other type queries. */
  bool isExternal() const {
    return (flags() & TYPE_FLAGS_MASK) == EXTERNAL_FLAGS;
  }

  MOZ_ALWAYS_INLINE
  JSExternalString& asExternal() const {
    MOZ_ASSERT(isExternal());
    return *(JSExternalString*)this;
  }

  MOZ_ALWAYS_INLINE
  bool isAtom() const { return flags() & ATOM_BIT; }

  MOZ_ALWAYS_INLINE
  bool isPermanentAtom() const {
    return (flags() & PERMANENT_ATOM_MASK) == PERMANENT_ATOM_MASK;
  }

  MOZ_ALWAYS_INLINE
  JSAtom& asAtom() const {
    MOZ_ASSERT(isAtom());
    return *(JSAtom*)this;
  }

  MOZ_ALWAYS_INLINE
  void setNonDeduplicatable() { setFlagBit(NON_DEDUP_BIT); }

  MOZ_ALWAYS_INLINE
  void clearNonDeduplicatable() { clearFlagBit(NON_DEDUP_BIT); }

  MOZ_ALWAYS_INLINE
  bool isDeduplicatable() { return !(flags() & NON_DEDUP_BIT); }

  void setInStringToAtomCache() {
    MOZ_ASSERT(!isAtom());
    setFlagBit(IN_STRING_TO_ATOM_CACHE);
  }
  bool inStringToAtomCache() const { return flags() & IN_STRING_TO_ATOM_CACHE; }

  // Fills |array| with various strings that represent the different string
  // kinds and character encodings.
  static bool fillWithRepresentatives(JSContext* cx,
                                      js::HandleArrayObject array);

  /* Only called by the GC for dependent strings. */

  inline bool hasBase() const { return isDependent(); }

  inline JSLinearString* base() const;

  // The base may be forwarded and becomes a relocation overlay.
  // The return value can be a relocation overlay when the base is forwarded,
  // or the return value can be the actual base when it is not forwarded.
  inline JSLinearString* nurseryBaseOrRelocOverlay() const;

  inline bool canOwnDependentChars() const;

  inline void setBase(JSLinearString* newBase);

  void traceBase(JSTracer* trc);

  /* Only called by the GC for strings with the AllocKind::STRING kind. */

  inline void finalize(JSFreeOp* fop);

  /* Gets the number of bytes that the chars take on the heap. */

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf);

  bool ownsMallocedChars() const {
    return isLinear() && !isInline() && !isDependent() && !isExternal();
  }

  /* Encode as many scalar values of the string as UTF-8 as can fit
   * into the caller-provided buffer replacing unpaired surrogates
   * with the REPLACEMENT CHARACTER.
   *
   * Returns the number of code units read and the number of code units
   * written.
   *
   * The semantics of this method match the semantics of
   * TextEncoder.encodeInto().
   *
   * This function doesn't modify the representation -- rope, linear,
   * flat, atom, etc. -- of this string. If this string is a rope,
   * it also doesn't modify the representation of left or right halves
   * of this string, or of those halves, and so on.
   *
   * Returns mozilla::Nothing on OOM.
   */
  mozilla::Maybe<mozilla::Tuple<size_t, size_t>> encodeUTF8Partial(
      const JS::AutoRequireNoGC& nogc, mozilla::Span<char> buffer) const;

 private:
  // To help avoid writing Spectre-unsafe code, we only allow MacroAssembler
  // to call the method below.
  friend class js::jit::MacroAssembler;
  static size_t offsetOfNonInlineChars() {
    static_assert(
        offsetof(JSString, d.s.u2.nonInlineCharsTwoByte) ==
            offsetof(JSString, d.s.u2.nonInlineCharsLatin1),
        "nonInlineCharsTwoByte and nonInlineCharsLatin1 must have same offset");
    return offsetof(JSString, d.s.u2.nonInlineCharsTwoByte);
  }

 public:
  static const JS::TraceKind TraceKind = JS::TraceKind::String;

  JS::Zone* zone() const {
    if (isTenured()) {
      // Allow permanent atoms to be accessed across zones and runtimes.
      if (isPermanentAtom()) {
        return zoneFromAnyThread();
      }
      return asTenured().zone();
    }
    return nurseryZone();
  }

  void setLengthAndFlags(uint32_t len, uint32_t flags) {
    setHeaderLengthAndFlags(len, flags);
  }
  void setFlagBit(uint32_t flag) { setHeaderFlagBit(flag); }
  void clearFlagBit(uint32_t flag) { clearHeaderFlagBit(flag); }

  void fixupAfterMovingGC() {}

  js::gc::AllocKind getAllocKind() const {
    using js::gc::AllocKind;
    AllocKind kind;
    if (isAtom()) {
      if (isFatInline()) {
        kind = AllocKind::FAT_INLINE_ATOM;
      } else {
        kind = AllocKind::ATOM;
      }
    } else if (isFatInline()) {
      kind = AllocKind::FAT_INLINE_STRING;
    } else if (isExternal()) {
      kind = AllocKind::EXTERNAL_STRING;
    } else {
      kind = AllocKind::STRING;
    }
    MOZ_ASSERT_IF(isTenured(), kind == asTenured().getAllocKind());
    return kind;
  }

#if defined(DEBUG) || defined(JS_JITSPEW)
  void dump();  // Debugger-friendly stderr dump.
  void dump(js::GenericPrinter& out);
  void dumpNoNewline(js::GenericPrinter& out);
  void dumpCharsNoNewline(js::GenericPrinter& out);
  void dumpRepresentation(js::GenericPrinter& out, int indent) const;
  void dumpRepresentationHeader(js::GenericPrinter& out,
                                const char* subclass) const;
  void dumpCharsNoQuote(js::GenericPrinter& out);

  template <typename CharT>
  static void dumpChars(const CharT* s, size_t len, js::GenericPrinter& out);

  template <typename CharT>
  static void dumpCharsNoQuote(const CharT* s, size_t len,
                               js::GenericPrinter& out);

  bool equals(const char* s);
#endif

  void traceChildren(JSTracer* trc);

  // Override base class implementation to tell GC about permanent atoms.
  bool isPermanentAndMayBeShared() const { return isPermanentAtom(); }

  static void addCellAddressToStoreBuffer(js::gc::StoreBuffer* buffer,
                                          js::gc::Cell** cellp) {
    buffer->putCell(reinterpret_cast<JSString**>(cellp));
  }

  static void removeCellAddressFromStoreBuffer(js::gc::StoreBuffer* buffer,
                                               js::gc::Cell** cellp) {
    buffer->unputCell(reinterpret_cast<JSString**>(cellp));
  }

 private:
  JSString() = delete;
  JSString(const JSString& other) = delete;
  void operator=(const JSString& other) = delete;
};

class JSRope : public JSString {
  template <typename CharT>
  js::UniquePtr<CharT[], JS::FreePolicy> copyCharsInternal(
      JSContext* cx, arena_id_t destArenaId) const;

  enum UsingBarrier : bool { NoBarrier = false, WithIncrementalBarrier = true };

  friend class JSString;
  JSLinearString* flatten(JSContext* maybecx);

  JSLinearString* flattenInternal();
  template <UsingBarrier usingBarrier>
  JSLinearString* flattenInternal();

  template <UsingBarrier usingBarrier, typename CharT>
  static JSLinearString* flattenInternal(JSRope* root);

  template <UsingBarrier usingBarrier>
  static void ropeBarrierDuringFlattening(JSRope* rope);

  void init(JSContext* cx, JSString* left, JSString* right, size_t length);

 public:
  template <js::AllowGC allowGC>
  static inline JSRope* new_(
      JSContext* cx,
      typename js::MaybeRooted<JSString*, allowGC>::HandleType left,
      typename js::MaybeRooted<JSString*, allowGC>::HandleType right,
      size_t length, js::gc::InitialHeap = js::gc::DefaultHeap);

  js::UniquePtr<JS::Latin1Char[], JS::FreePolicy> copyLatin1Chars(
      JSContext* maybecx, arena_id_t destArenaId) const;
  JS::UniqueTwoByteChars copyTwoByteChars(JSContext* maybecx,
                                          arena_id_t destArenaId) const;

  template <typename CharT>
  js::UniquePtr<CharT[], JS::FreePolicy> copyChars(
      JSContext* maybecx, arena_id_t destArenaId) const;

  // Hash function specific for ropes that avoids allocating a temporary
  // string. There are still allocations internally so it's technically
  // fallible.
  //
  // Returns the same value as if this were a linear string being hashed.
  [[nodiscard]] bool hash(uint32_t* outhHash) const;

  // The process of flattening a rope temporarily overwrites the left pointer of
  // interior nodes in the rope DAG with the parent pointer.
  bool isBeingFlattened() const { return flags() & FLATTEN_MASK; }

  JSString* leftChild() const {
    MOZ_ASSERT(isRope());
    MOZ_ASSERT(!isBeingFlattened());  // Flattening overwrites this field.
    return d.s.u2.left;
  }

  JSString* rightChild() const {
    MOZ_ASSERT(isRope());
    return d.s.u3.right;
  }

  void traceChildren(JSTracer* trc);

#if defined(DEBUG) || defined(JS_JITSPEW)
  void dumpRepresentation(js::GenericPrinter& out, int indent) const;
#endif

 private:
  // To help avoid writing Spectre-unsafe code, we only allow MacroAssembler
  // to call the methods below.
  friend class js::jit::MacroAssembler;

  static size_t offsetOfLeft() { return offsetof(JSRope, d.s.u2.left); }
  static size_t offsetOfRight() { return offsetof(JSRope, d.s.u3.right); }
};

static_assert(sizeof(JSRope) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

class JSLinearString : public JSString {
  friend class JSString;
  friend class JS::AutoStableStringChars;
  friend class js::TenuringTracer;

  /* Vacuous and therefore unimplemented. */
  JSLinearString* ensureLinear(JSContext* cx) = delete;
  bool isLinear() const = delete;
  JSLinearString& asLinear() const = delete;

 protected:
  /* Returns void pointer to latin1/twoByte chars, for finalizers. */
  MOZ_ALWAYS_INLINE
  void* nonInlineCharsRaw() const {
    MOZ_ASSERT(!isInline());
    static_assert(
        offsetof(JSLinearString, d.s.u2.nonInlineCharsTwoByte) ==
            offsetof(JSLinearString, d.s.u2.nonInlineCharsLatin1),
        "nonInlineCharsTwoByte and nonInlineCharsLatin1 must have same offset");
    return (void*)d.s.u2.nonInlineCharsTwoByte;
  }

  MOZ_ALWAYS_INLINE const JS::Latin1Char* rawLatin1Chars() const;
  MOZ_ALWAYS_INLINE const char16_t* rawTwoByteChars() const;

 public:
  void init(const char16_t* chars, size_t length);
  void init(const JS::Latin1Char* chars, size_t length);

  template <js::AllowGC allowGC, typename CharT>
  static inline JSLinearString* new_(
      JSContext* cx, js::UniquePtr<CharT[], JS::FreePolicy> chars,
      size_t length, js::gc::InitialHeap heap);

  template <typename CharT>
  MOZ_ALWAYS_INLINE const CharT* nonInlineChars(
      const JS::AutoRequireNoGC& nogc) const;

  MOZ_ALWAYS_INLINE
  const JS::Latin1Char* nonInlineLatin1Chars(
      const JS::AutoRequireNoGC& nogc) const {
    MOZ_ASSERT(!isInline());
    MOZ_ASSERT(hasLatin1Chars());
    return d.s.u2.nonInlineCharsLatin1;
  }

  MOZ_ALWAYS_INLINE
  const char16_t* nonInlineTwoByteChars(const JS::AutoRequireNoGC& nogc) const {
    MOZ_ASSERT(!isInline());
    MOZ_ASSERT(hasTwoByteChars());
    return d.s.u2.nonInlineCharsTwoByte;
  }

  template <typename CharT>
  MOZ_ALWAYS_INLINE const CharT* chars(const JS::AutoRequireNoGC& nogc) const;

  MOZ_ALWAYS_INLINE
  const JS::Latin1Char* latin1Chars(const JS::AutoRequireNoGC& nogc) const {
    return rawLatin1Chars();
  }

  MOZ_ALWAYS_INLINE
  const char16_t* twoByteChars(const JS::AutoRequireNoGC& nogc) const {
    return rawTwoByteChars();
  }

  mozilla::Range<const JS::Latin1Char> latin1Range(
      const JS::AutoRequireNoGC& nogc) const {
    MOZ_ASSERT(JSString::isLinear());
    return mozilla::Range<const JS::Latin1Char>(latin1Chars(nogc), length());
  }

  mozilla::Range<const char16_t> twoByteRange(
      const JS::AutoRequireNoGC& nogc) const {
    MOZ_ASSERT(JSString::isLinear());
    return mozilla::Range<const char16_t>(twoByteChars(nogc), length());
  }

  MOZ_ALWAYS_INLINE
  char16_t latin1OrTwoByteChar(size_t index) const {
    MOZ_ASSERT(JSString::isLinear());
    MOZ_ASSERT(index < length());
    JS::AutoCheckCannotGC nogc;
    return hasLatin1Chars() ? latin1Chars(nogc)[index]
                            : twoByteChars(nogc)[index];
  }

  bool isIndexSlow(uint32_t* indexp) const {
    MOZ_ASSERT(JSString::isLinear());
    size_t len = length();
    if (len == 0 || len > js::UINT32_CHAR_BUFFER_LENGTH) {
      return false;
    }
    JS::AutoCheckCannotGC nogc;
    if (hasLatin1Chars()) {
      const JS::Latin1Char* s = latin1Chars(nogc);
      return mozilla::IsAsciiDigit(*s) &&
             js::CheckStringIsIndex(s, len, indexp);
    }
    const char16_t* s = twoByteChars(nogc);
    return mozilla::IsAsciiDigit(*s) && js::CheckStringIsIndex(s, len, indexp);
  }

  // Returns true if this string's characters store an unsigned 32-bit integer
  // value less than or equal to MAX_ARRAY_INDEX, initializing *indexp to that
  // value if so. Leading '0' isn't allowed except 0 itself.
  // (Thus if calling isIndex returns true, js::IndexToString(cx, *indexp) will
  // be a string equal to this string.)
  inline bool isIndex(uint32_t* indexp) const;

  void maybeInitializeIndexValue(uint32_t index, bool allowAtom = false) {
    MOZ_ASSERT(JSString::isLinear());
    MOZ_ASSERT_IF(hasIndexValue(), getIndexValue() == index);
    MOZ_ASSERT_IF(!allowAtom, !isAtom());

    if (hasIndexValue() || index > UINT16_MAX) {
      return;
    }

    mozilla::DebugOnly<uint32_t> containedIndex;
    MOZ_ASSERT(isIndexSlow(&containedIndex));
    MOZ_ASSERT(index == containedIndex);

    setFlagBit((index << INDEX_VALUE_SHIFT) | INDEX_VALUE_BIT);
    MOZ_ASSERT(getIndexValue() == index);
  }

  /*
   * Returns a property name represented by this string, or null on failure.
   * You must verify that this is not an index per isIndex before calling
   * this method.
   */
  inline js::PropertyName* toPropertyName(JSContext* cx);

  inline void finalize(JSFreeOp* fop);
  inline size_t allocSize() const;

#if defined(DEBUG) || defined(JS_JITSPEW)
  void dumpRepresentationChars(js::GenericPrinter& out, int indent) const;
  void dumpRepresentation(js::GenericPrinter& out, int indent) const;
#endif

  /*
   * Once a JSLinearString sub-class has been added to the atom state, this
   * operation changes the string to the JSAtom type, in place.
   */
  MOZ_ALWAYS_INLINE JSAtom* morphAtomizedStringIntoAtom(js::HashNumber hash);
  MOZ_ALWAYS_INLINE JSAtom* morphAtomizedStringIntoPermanentAtom(
      js::HashNumber hash);
};

static_assert(sizeof(JSLinearString) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

class JSDependentString : public JSLinearString {
  friend class JSString;

  void init(JSContext* cx, JSLinearString* base, size_t start, size_t length);

  /* Vacuous and therefore unimplemented. */
  bool isDependent() const = delete;
  JSDependentString& asDependent() const = delete;

  /* The offset of this string's chars in base->chars(). */
  MOZ_ALWAYS_INLINE size_t baseOffset() const {
    MOZ_ASSERT(JSString::isDependent());
    JS::AutoCheckCannotGC nogc;
    size_t offset;
    if (hasTwoByteChars()) {
      offset = twoByteChars(nogc) - base()->twoByteChars(nogc);
    } else {
      offset = latin1Chars(nogc) - base()->latin1Chars(nogc);
    }
    MOZ_ASSERT(offset < base()->length());
    return offset;
  }

 public:
  static inline JSLinearString* new_(JSContext* cx, JSLinearString* base,
                                     size_t start, size_t length,
                                     js::gc::InitialHeap heap);

  template <typename T>
  void relocateNonInlineChars(T chars, size_t offset) {
    setNonInlineChars(chars + offset);
  }

#if defined(DEBUG) || defined(JS_JITSPEW)
  void dumpRepresentation(js::GenericPrinter& out, int indent) const;
#endif

 private:
  // To help avoid writing Spectre-unsafe code, we only allow MacroAssembler
  // to call the method below.
  friend class js::jit::MacroAssembler;

  inline static size_t offsetOfBase() {
    return offsetof(JSDependentString, d.s.u3.base);
  }
};

static_assert(sizeof(JSDependentString) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

class JSExtensibleString : public JSLinearString {
  /* Vacuous and therefore unimplemented. */
  bool isExtensible() const = delete;
  JSExtensibleString& asExtensible() const = delete;

 public:
  MOZ_ALWAYS_INLINE
  size_t capacity() const {
    MOZ_ASSERT(JSString::isExtensible());
    return d.s.u3.capacity;
  }

#if defined(DEBUG) || defined(JS_JITSPEW)
  void dumpRepresentation(js::GenericPrinter& out, int indent) const;
#endif
};

static_assert(sizeof(JSExtensibleString) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

class JSInlineString : public JSLinearString {
 public:
  MOZ_ALWAYS_INLINE
  const JS::Latin1Char* latin1Chars(const JS::AutoRequireNoGC& nogc) const {
    MOZ_ASSERT(JSString::isInline());
    MOZ_ASSERT(hasLatin1Chars());
    return d.inlineStorageLatin1;
  }

  MOZ_ALWAYS_INLINE
  const char16_t* twoByteChars(const JS::AutoRequireNoGC& nogc) const {
    MOZ_ASSERT(JSString::isInline());
    MOZ_ASSERT(hasTwoByteChars());
    return d.inlineStorageTwoByte;
  }

  template <typename CharT>
  static bool lengthFits(size_t length);

#if defined(DEBUG) || defined(JS_JITSPEW)
  void dumpRepresentation(js::GenericPrinter& out, int indent) const;
#endif

 private:
  // To help avoid writing Spectre-unsafe code, we only allow MacroAssembler
  // to call the method below.
  friend class js::jit::MacroAssembler;
  static size_t offsetOfInlineStorage() {
    return offsetof(JSInlineString, d.inlineStorageTwoByte);
  }
};

static_assert(sizeof(JSInlineString) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

/*
 * On 32-bit platforms, JSThinInlineString can store 8 Latin1 characters or 4
 * TwoByte characters inline. On 64-bit platforms, these numbers are 16 and 8,
 * respectively.
 */
class JSThinInlineString : public JSInlineString {
 public:
  static const size_t MAX_LENGTH_LATIN1 = NUM_INLINE_CHARS_LATIN1;
  static const size_t MAX_LENGTH_TWO_BYTE = NUM_INLINE_CHARS_TWO_BYTE;

  template <js::AllowGC allowGC>
  static inline JSThinInlineString* new_(JSContext* cx,
                                         js::gc::InitialHeap heap);

  template <typename CharT>
  inline CharT* init(size_t length);

  template <typename CharT>
  static bool lengthFits(size_t length);
};

static_assert(sizeof(JSThinInlineString) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

/*
 * On both 32-bit and 64-bit platforms, MAX_LENGTH_TWO_BYTE is 12 and
 * MAX_LENGTH_LATIN1 is 24. This is deliberate, in order to minimize potential
 * performance differences between 32-bit and 64-bit platforms.
 *
 * There are still some differences due to NUM_INLINE_CHARS_* being different.
 * E.g. TwoByte strings of length 5--8 will be JSFatInlineStrings on 32-bit
 * platforms and JSThinInlineStrings on 64-bit platforms. But the more
 * significant transition from inline strings to non-inline strings occurs at
 * length 12 (for TwoByte strings) and 24 (Latin1 strings) on both 32-bit and
 * 64-bit platforms.
 */
class JSFatInlineString : public JSInlineString {
  static const size_t INLINE_EXTENSION_CHARS_LATIN1 =
      24 - NUM_INLINE_CHARS_LATIN1;
  static const size_t INLINE_EXTENSION_CHARS_TWO_BYTE =
      12 - NUM_INLINE_CHARS_TWO_BYTE;

 protected: /* to fool clang into not warning this is unused */
  union {
    char inlineStorageExtensionLatin1[INLINE_EXTENSION_CHARS_LATIN1];
    char16_t inlineStorageExtensionTwoByte[INLINE_EXTENSION_CHARS_TWO_BYTE];
  };

 public:
  template <js::AllowGC allowGC>
  static inline JSFatInlineString* new_(JSContext* cx,
                                        js::gc::InitialHeap heap);

  static const size_t MAX_LENGTH_LATIN1 =
      JSString::NUM_INLINE_CHARS_LATIN1 + INLINE_EXTENSION_CHARS_LATIN1;

  static const size_t MAX_LENGTH_TWO_BYTE =
      JSString::NUM_INLINE_CHARS_TWO_BYTE + INLINE_EXTENSION_CHARS_TWO_BYTE;

  template <typename CharT>
  inline CharT* init(size_t length);

  template <typename CharT>
  static bool lengthFits(size_t length);

  // Only called by the GC for strings with the AllocKind::FAT_INLINE_STRING
  // kind.
  MOZ_ALWAYS_INLINE void finalize(JSFreeOp* fop);
};

static_assert(sizeof(JSFatInlineString) % js::gc::CellAlignBytes == 0,
              "fat inline strings shouldn't waste space up to the next cell "
              "boundary");

class JSExternalString : public JSLinearString {
  void init(const char16_t* chars, size_t length,
            const JSExternalStringCallbacks* callbacks);

  /* Vacuous and therefore unimplemented. */
  bool isExternal() const = delete;
  JSExternalString& asExternal() const = delete;

 public:
  static inline JSExternalString* new_(
      JSContext* cx, const char16_t* chars, size_t length,
      const JSExternalStringCallbacks* callbacks);

  const JSExternalStringCallbacks* callbacks() const {
    MOZ_ASSERT(JSString::isExternal());
    return d.s.u3.externalCallbacks;
  }

  // External chars are never allocated inline or in the nursery, so we can
  // safely expose this without requiring an AutoCheckCannotGC argument.
  const char16_t* twoByteChars() const { return rawTwoByteChars(); }

  // Only called by the GC for strings with the AllocKind::EXTERNAL_STRING
  // kind.
  inline void finalize(JSFreeOp* fop);

#if defined(DEBUG) || defined(JS_JITSPEW)
  void dumpRepresentation(js::GenericPrinter& out, int indent) const;
#endif
};

static_assert(sizeof(JSExternalString) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

class JSAtom : public JSLinearString {
  /* Vacuous and therefore unimplemented. */
  bool isAtom() const = delete;
  JSAtom& asAtom() const = delete;

 public:
  /* Returns the PropertyName for this.  isIndex() must be false. */
  inline js::PropertyName* asPropertyName();

  MOZ_ALWAYS_INLINE
  bool isPermanent() const { return JSString::isPermanentAtom(); }

  // Transform this atom into a permanent atom. This is only done during
  // initialization of the runtime. Permanent atoms are always pinned.
  MOZ_ALWAYS_INLINE void morphIntoPermanentAtom() {
    MOZ_ASSERT(static_cast<JSString*>(this)->isAtom());
    setFlagBit(PERMANENT_ATOM_MASK);
  }

  MOZ_ALWAYS_INLINE bool isIndex() const {
    MOZ_ASSERT(JSString::isAtom());
    mozilla::DebugOnly<uint32_t> index;
    MOZ_ASSERT(!!(flags() & ATOM_IS_INDEX_BIT) == isIndexSlow(&index));
    return flags() & ATOM_IS_INDEX_BIT;
  }
  MOZ_ALWAYS_INLINE bool isIndex(uint32_t* index) const {
    MOZ_ASSERT(JSString::isAtom());
    if (!isIndex()) {
      return false;
    }
    *index = hasIndexValue() ? getIndexValue() : getIndexSlow();
    return true;
  }

  uint32_t getIndexSlow() const;

  void setIsIndex(uint32_t index) {
    MOZ_ASSERT(JSString::isAtom());
    setFlagBit(ATOM_IS_INDEX_BIT);
    maybeInitializeIndexValue(index, /* allowAtom = */ true);
  }

  inline js::HashNumber hash() const;
  inline void initHash(js::HashNumber hash);

#if defined(DEBUG) || defined(JS_JITSPEW)
  void dump(js::GenericPrinter& out);
  void dump();
#endif
};

static_assert(sizeof(JSAtom) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

namespace js {

class NormalAtom : public JSAtom {
 protected:
  HashNumber hash_;

 public:
  HashNumber hash() const { return hash_; }
  void initHash(HashNumber hash) { hash_ = hash; }
};

static_assert(sizeof(NormalAtom) == sizeof(JSString) + sizeof(uint64_t),
              "NormalAtom must have size of a string + HashNumber, "
              "aligned to gc::CellAlignBytes");

class FatInlineAtom : public JSAtom {
 protected:  // Silence Clang unused-field warning.
  char inlineStorage_[sizeof(JSFatInlineString) - sizeof(JSString)];
  HashNumber hash_;

 public:
  HashNumber hash() const { return hash_; }
  void initHash(HashNumber hash) { hash_ = hash; }

  inline void finalize(JSFreeOp* fop);
};

static_assert(
    sizeof(FatInlineAtom) == sizeof(JSFatInlineString) + sizeof(uint64_t),
    "FatInlineAtom must have size of a fat inline string + HashNumber, "
    "aligned to gc::CellAlignBytes");

}  // namespace js

inline js::HashNumber JSAtom::hash() const {
  if (isFatInline()) {
    return static_cast<const js::FatInlineAtom*>(this)->hash();
  }
  return static_cast<const js::NormalAtom*>(this)->hash();
}

inline void JSAtom::initHash(js::HashNumber hash) {
  if (isFatInline()) {
    return static_cast<js::FatInlineAtom*>(this)->initHash(hash);
  }
  return static_cast<js::NormalAtom*>(this)->initHash(hash);
}

MOZ_ALWAYS_INLINE JSAtom* JSLinearString::morphAtomizedStringIntoAtom(
    js::HashNumber hash) {
  MOZ_ASSERT(!isAtom());
  setFlagBit(ATOM_BIT);
  JSAtom* atom = &asAtom();
  atom->initHash(hash);
  return atom;
}

MOZ_ALWAYS_INLINE JSAtom* JSLinearString::morphAtomizedStringIntoPermanentAtom(
    js::HashNumber hash) {
  MOZ_ASSERT(!isAtom());
  setFlagBit(PERMANENT_ATOM_MASK);
  JSAtom* atom = &asAtom();
  atom->initHash(hash);
  return atom;
}

namespace js {

/**
 * An indexable characters class exposing unaligned, little-endian encoded
 * char16_t data.
 */
class LittleEndianChars {
 public:
  explicit constexpr LittleEndianChars(const uint8_t* leTwoByte)
      : current(leTwoByte) {}

  constexpr char16_t operator[](size_t index) const {
    size_t offset = index * sizeof(char16_t);
    return (current[offset + 1] << 8) | current[offset];
  }

  constexpr const uint8_t* get() { return current; }

 private:
  const uint8_t* current;
};

class StaticStrings {
  // NOTE: The WellKnownParserAtoms rely on these tables and may need to be
  //       update if these tables are changed.
  friend class js::frontend::ParserAtomsTable;
  friend class js::frontend::TaggedParserAtomIndex;
  friend class js::frontend::WellKnownParserAtoms;
  friend struct js::frontend::CompilationAtomCache;

 private:
  // Strings matches `[A-Za-z0-9$_]{2}` pattern.
  // Store each character in 6 bits.
  // See fromSmallChar/toSmallChar for the mapping.
  static constexpr size_t SMALL_CHAR_BITS = 6;
  static constexpr size_t SMALL_CHAR_MASK = js::BitMask(SMALL_CHAR_BITS);

  // To optimize ASCII -> small char, allocate a table.
  static constexpr size_t SMALL_CHAR_TABLE_SIZE = 128U;
  static constexpr size_t NUM_SMALL_CHARS = js::Bit(SMALL_CHAR_BITS);
  static constexpr size_t NUM_LENGTH2_ENTRIES =
      NUM_SMALL_CHARS * NUM_SMALL_CHARS;

  JSAtom* length2StaticTable[NUM_LENGTH2_ENTRIES] = {};  // zeroes

 public:
  /* We keep these public for the JITs. */
  static const size_t UNIT_STATIC_LIMIT = 256U;
  JSAtom* unitStaticTable[UNIT_STATIC_LIMIT] = {};  // zeroes

  static const size_t INT_STATIC_LIMIT = 256U;
  JSAtom* intStaticTable[INT_STATIC_LIMIT] = {};  // zeroes

  StaticStrings() = default;

  bool init(JSContext* cx);
  void trace(JSTracer* trc);

  static bool hasUint(uint32_t u) { return u < INT_STATIC_LIMIT; }

  JSAtom* getUint(uint32_t u) {
    MOZ_ASSERT(hasUint(u));
    return intStaticTable[u];
  }

  static bool hasInt(int32_t i) { return uint32_t(i) < INT_STATIC_LIMIT; }

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
  inline JSLinearString* getUnitStringForElement(JSContext* cx, JSString* str,
                                                 size_t index);

  template <typename CharT>
  static bool isStatic(const CharT* chars, size_t len);

  /* Return null if no static atom exists for the given (chars, length). */
  template <typename Chars>
  MOZ_ALWAYS_INLINE JSAtom* lookup(Chars chars, size_t length) {
    static_assert(std::is_same_v<Chars, const Latin1Char*> ||
                      std::is_same_v<Chars, const char16_t*> ||
                      std::is_same_v<Chars, LittleEndianChars>,
                  "for understandability, |chars| must be one of a few "
                  "identified types");

    switch (length) {
      case 1: {
        char16_t c = chars[0];
        if (c < UNIT_STATIC_LIMIT) {
          return getUnit(c);
        }
        return nullptr;
      }
      case 2:
        if (fitsInSmallChar(chars[0]) && fitsInSmallChar(chars[1])) {
          return getLength2(chars[0], chars[1]);
        }
        return nullptr;
      case 3:
        /*
         * Here we know that JSString::intStringTable covers only 256 (or at
         * least not 1000 or more) chars. We rely on order here to resolve the
         * unit vs. int string/length-2 string atom identity issue by giving
         * priority to unit strings for "0" through "9" and length-2 strings for
         * "10" through "99".
         */
        static_assert(INT_STATIC_LIMIT <= 999,
                      "static int strings assumed below to be at most "
                      "three digits");
        if ('1' <= chars[0] && chars[0] <= '9' && '0' <= chars[1] &&
            chars[1] <= '9' && '0' <= chars[2] && chars[2] <= '9') {
          int i =
              (chars[0] - '0') * 100 + (chars[1] - '0') * 10 + (chars[2] - '0');

          if (unsigned(i) < INT_STATIC_LIMIT) {
            return getInt(i);
          }
        }
        return nullptr;
    }

    return nullptr;
  }

  MOZ_ALWAYS_INLINE JSAtom* lookup(const char* chars, size_t length) {
    // Collapse calls for |const char*| into |const Latin1Char char*| to avoid
    // excess instantiations.
    return lookup(reinterpret_cast<const Latin1Char*>(chars), length);
  }

  template <typename CharT,
            typename = std::enable_if_t<!std::is_const_v<CharT>>>
  MOZ_ALWAYS_INLINE JSAtom* lookup(CharT* chars, size_t length) {
    // Collapse the remaining |CharT*| to |const CharT*| to avoid excess
    // instantiations.
    return lookup(const_cast<const CharT*>(chars), length);
  }

 private:
  using SmallChar = uint8_t;

  struct SmallCharTable {
    SmallChar storage[SMALL_CHAR_TABLE_SIZE];

    constexpr SmallChar& operator[](size_t idx) { return storage[idx]; }
    constexpr const SmallChar& operator[](size_t idx) const {
      return storage[idx];
    }
  };

  static const SmallChar INVALID_SMALL_CHAR = -1;

  static bool fitsInSmallChar(char16_t c) {
    return c < SMALL_CHAR_TABLE_SIZE &&
           toSmallCharTable[c] != INVALID_SMALL_CHAR;
  }

  static constexpr Latin1Char fromSmallChar(SmallChar c);

  static constexpr SmallChar toSmallChar(uint32_t c);

  static constexpr SmallCharTable createSmallCharTable();

  static const SmallCharTable toSmallCharTable;

  static constexpr Latin1Char firstCharOfLength2(size_t s) {
    return fromSmallChar(s >> SMALL_CHAR_BITS);
  }
  static constexpr Latin1Char secondCharOfLength2(size_t s) {
    return fromSmallChar(s & SMALL_CHAR_MASK);
  }

  static MOZ_ALWAYS_INLINE size_t getLength2Index(char16_t c1, char16_t c2) {
    MOZ_ASSERT(fitsInSmallChar(c1));
    MOZ_ASSERT(fitsInSmallChar(c2));
    return (size_t(toSmallCharTable[c1]) << SMALL_CHAR_BITS) +
           toSmallCharTable[c2];
  }

  // Same as getLength2Index, but withtout runtime assertion,
  // this should be used only for known static string.
  static constexpr size_t getLength2IndexStatic(char c1, char c2) {
    return (size_t(toSmallChar(c1)) << SMALL_CHAR_BITS) + toSmallChar(c2);
  }

  MOZ_ALWAYS_INLINE JSAtom* getLength2FromIndex(size_t index) {
    return length2StaticTable[index];
  }

  MOZ_ALWAYS_INLINE JSAtom* getLength2(char16_t c1, char16_t c2) {
    return getLength2FromIndex(getLength2Index(c1, c2));
  }
};

/*
 * Declare length-2 strings. We only store strings where both characters are
 * alphanumeric. The lower 10 short chars are the numerals, the next 26 are
 * the lowercase letters, and the next 26 are the uppercase letters.
 */

constexpr Latin1Char StaticStrings::fromSmallChar(SmallChar c) {
  if (c < 10) {
    return c + '0';
  }
  if (c < 36) {
    return c + 'a' - 10;
  }
  if (c < 62) {
    return c + 'A' - 36;
  }
  if (c == 62) {
    return '$';
  }
  return '_';
}

constexpr StaticStrings::SmallChar StaticStrings::toSmallChar(uint32_t c) {
  if (mozilla::IsAsciiDigit(c)) {
    return c - '0';
  }
  if (mozilla::IsAsciiLowercaseAlpha(c)) {
    return c - 'a' + 10;
  }
  if (mozilla::IsAsciiUppercaseAlpha(c)) {
    return c - 'A' + 36;
  }
  if (c == '$') {
    return 62;
  }
  if (c == '_') {
    return 63;
  }
  return StaticStrings::INVALID_SMALL_CHAR;
}

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
class PropertyName : public JSAtom {
 private:
  /* Vacuous and therefore unimplemented. */
  PropertyName* asPropertyName() = delete;
};

static_assert(sizeof(PropertyName) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

static MOZ_ALWAYS_INLINE jsid NameToId(PropertyName* name) {
  return JS::PropertyKey::fromNonIntAtom(name);
}

using PropertyNameVector = JS::GCVector<PropertyName*>;

template <typename CharT>
void CopyChars(CharT* dest, const JSLinearString& str);

static inline UniqueChars StringToNewUTF8CharsZ(JSContext* cx, JSString& str) {
  JS::AutoCheckCannotGC nogc;

  JSLinearString* linear = str.ensureLinear(cx);
  if (!linear) {
    return nullptr;
  }

  return UniqueChars(
      linear->hasLatin1Chars()
          ? JS::CharsToNewUTF8CharsZ(cx, linear->latin1Range(nogc)).c_str()
          : JS::CharsToNewUTF8CharsZ(cx, linear->twoByteRange(nogc)).c_str());
}

/**
 * Allocate a string with the given contents.  If |allowGC == CanGC|, this may
 * trigger a GC.
 */
template <js::AllowGC allowGC, typename CharT>
extern JSLinearString* NewString(
    JSContext* cx, UniquePtr<CharT[], JS::FreePolicy> chars, size_t length,
    js::gc::InitialHeap heap = js::gc::DefaultHeap);

/* Like NewString, but doesn't try to deflate to Latin1. */
template <js::AllowGC allowGC, typename CharT>
extern JSLinearString* NewStringDontDeflate(
    JSContext* cx, UniquePtr<CharT[], JS::FreePolicy> chars, size_t length,
    js::gc::InitialHeap heap = js::gc::DefaultHeap);

extern JSLinearString* NewDependentString(
    JSContext* cx, JSString* base, size_t start, size_t length,
    js::gc::InitialHeap heap = js::gc::DefaultHeap);

/* Take ownership of an array of Latin1Chars. */
extern JSLinearString* NewLatin1StringZ(
    JSContext* cx, UniqueChars chars,
    js::gc::InitialHeap heap = js::gc::DefaultHeap);

/* Copy a counted string and GC-allocate a descriptor for it. */
template <js::AllowGC allowGC, typename CharT>
extern JSLinearString* NewStringCopyN(
    JSContext* cx, const CharT* s, size_t n,
    js::gc::InitialHeap heap = js::gc::DefaultHeap);

template <js::AllowGC allowGC>
inline JSLinearString* NewStringCopyN(
    JSContext* cx, const char* s, size_t n,
    js::gc::InitialHeap heap = js::gc::DefaultHeap) {
  return NewStringCopyN<allowGC>(cx, reinterpret_cast<const Latin1Char*>(s), n,
                                 heap);
}

/* Like NewStringCopyN, but doesn't try to deflate to Latin1. */
template <js::AllowGC allowGC, typename CharT>
extern JSLinearString* NewStringCopyNDontDeflate(
    JSContext* cx, const CharT* s, size_t n,
    js::gc::InitialHeap heap = js::gc::DefaultHeap);

/* Copy a C string and GC-allocate a descriptor for it. */
template <js::AllowGC allowGC>
inline JSLinearString* NewStringCopyZ(
    JSContext* cx, const char16_t* s,
    js::gc::InitialHeap heap = js::gc::DefaultHeap) {
  return NewStringCopyN<allowGC>(cx, s, js_strlen(s), heap);
}

template <js::AllowGC allowGC>
inline JSLinearString* NewStringCopyZ(
    JSContext* cx, const char* s,
    js::gc::InitialHeap heap = js::gc::DefaultHeap) {
  return NewStringCopyN<allowGC>(cx, s, strlen(s), heap);
}

template <js::AllowGC allowGC>
extern JSLinearString* NewStringCopyUTF8N(
    JSContext* cx, const JS::UTF8Chars utf8,
    js::gc::InitialHeap heap = js::gc::DefaultHeap);

template <js::AllowGC allowGC>
inline JSLinearString* NewStringCopyUTF8Z(
    JSContext* cx, const JS::ConstUTF8CharsZ utf8,
    js::gc::InitialHeap heap = js::gc::DefaultHeap) {
  return NewStringCopyUTF8N<allowGC>(
      cx, JS::UTF8Chars(utf8.c_str(), strlen(utf8.c_str())), heap);
}

JSString* NewMaybeExternalString(
    JSContext* cx, const char16_t* s, size_t n,
    const JSExternalStringCallbacks* callbacks, bool* allocatedExternal,
    js::gc::InitialHeap heap = js::gc::DefaultHeap);

/**
 * Allocate a new string consisting of |chars[0..length]| characters.
 */
extern JSLinearString* NewStringFromLittleEndianNoGC(
    JSContext* cx, LittleEndianChars chars, size_t length,
    js::gc::InitialHeap heap = js::gc::DefaultHeap);

static_assert(sizeof(HashNumber) == 4);

template <AllowGC allowGC>
extern JSString* ConcatStrings(
    JSContext* cx, typename MaybeRooted<JSString*, allowGC>::HandleType left,
    typename MaybeRooted<JSString*, allowGC>::HandleType right,
    js::gc::InitialHeap heap = js::gc::DefaultHeap);

/*
 * Test if strings are equal. The caller can call the function even if str1
 * or str2 are not GC-allocated things.
 */
extern bool EqualStrings(JSContext* cx, JSString* str1, JSString* str2,
                         bool* result);

/* Use the infallible method instead! */
extern bool EqualStrings(JSContext* cx, JSLinearString* str1,
                         JSLinearString* str2, bool* result) = delete;

/* EqualStrings is infallible on linear strings. */
extern bool EqualStrings(JSLinearString* str1, JSLinearString* str2);

/**
 * Compare two strings that are known to be the same length.
 * Exposed for the JITs; for ordinary uses, EqualStrings() is more sensible.
 *
 * Precondition: str1->length() == str2->length().
 */
extern bool EqualChars(JSLinearString* str1, JSLinearString* str2);

/*
 * Return less than, equal to, or greater than zero depending on whether
 * `s1[0..len1]` is less than, equal to, or greater than `s2`.
 */
extern int32_t CompareChars(const char16_t* s1, size_t len1,
                            JSLinearString* s2);

/*
 * Compare two strings, like CompareChars, but store the result in `*result`.
 * This flattens the strings and therefore can fail.
 */
extern bool CompareStrings(JSContext* cx, JSString* str1, JSString* str2,
                           int32_t* result);

/*
 * Same as CompareStrings but for atoms.  Don't use this to just test
 * for equality; use this when you need an ordering on atoms.
 */
extern int32_t CompareAtoms(JSAtom* atom1, JSAtom* atom2);

/**
 * Return true if the string contains only ASCII characters.
 */
extern bool StringIsAscii(JSLinearString* str);

/*
 * Return true if the string matches the given sequence of ASCII bytes.
 */
extern bool StringEqualsAscii(JSLinearString* str, const char* asciiBytes);
/*
 * Return true if the string matches the given sequence of ASCII
 * bytes.  The sequence of ASCII bytes must have length "length".  The
 * length should not include the trailing null, if any.
 */
extern bool StringEqualsAscii(JSLinearString* str, const char* asciiBytes,
                              size_t length);

template <size_t N>
bool StringEqualsLiteral(JSLinearString* str, const char (&asciiBytes)[N]) {
  MOZ_ASSERT(asciiBytes[N - 1] == '\0');
  return StringEqualsAscii(str, asciiBytes, N - 1);
}

extern int StringFindPattern(JSLinearString* text, JSLinearString* pat,
                             size_t start);

/**
 * Return true if the string contains a pattern at |start|.
 *
 * Precondition: `text` is long enough that this might be true;
 * that is, it has at least `start + pat->length()` characters.
 */
extern bool HasSubstringAt(JSLinearString* text, JSLinearString* pat,
                           size_t start);

/*
 * Computes |str|'s substring for the range [beginInt, beginInt + lengthInt).
 * Negative, overlarge, swapped, etc. |beginInt| and |lengthInt| are forbidden
 * and constitute API misuse.
 */
JSString* SubstringKernel(JSContext* cx, HandleString str, int32_t beginInt,
                          int32_t lengthInt);

inline js::HashNumber HashStringChars(JSLinearString* str) {
  JS::AutoCheckCannotGC nogc;
  size_t len = str->length();
  return str->hasLatin1Chars()
             ? mozilla::HashString(str->latin1Chars(nogc), len)
             : mozilla::HashString(str->twoByteChars(nogc), len);
}

/*** Conversions ************************************************************/

/*
 * Convert a string to a printable C string.
 *
 * Asserts if the input contains any non-ASCII characters.
 */
UniqueChars EncodeAscii(JSContext* cx, JSString* str);

/*
 * Convert a string to a printable C string.
 */
UniqueChars EncodeLatin1(JSContext* cx, JSString* str);

enum class IdToPrintableBehavior : bool {
  /*
   * Request the printable representation of an identifier.
   */
  IdIsIdentifier,

  /*
   * Request the printable representation of a property key.
   */
  IdIsPropertyKey
};

/*
 * Convert a jsid to a printable C string encoded in UTF-8.
 */
extern UniqueChars IdToPrintableUTF8(JSContext* cx, HandleId id,
                                     IdToPrintableBehavior behavior);

/*
 * Convert a non-string value to a string, returning null after reporting an
 * error, otherwise returning a new string reference.
 */
template <AllowGC allowGC>
extern JSString* ToStringSlow(
    JSContext* cx, typename MaybeRooted<Value, allowGC>::HandleType arg);

/*
 * Convert the given value to a string.  This method includes an inline
 * fast-path for the case where the value is already a string; if the value is
 * known not to be a string, use ToStringSlow instead.
 */
template <AllowGC allowGC>
static MOZ_ALWAYS_INLINE JSString* ToString(JSContext* cx, JS::HandleValue v) {
  if (v.isString()) {
    return v.toString();
  }
  return ToStringSlow<allowGC>(cx, v);
}

/*
 * This function implements E-262-3 section 9.8, toString. Convert the given
 * value to a string of characters appended to the given buffer. On error, the
 * passed buffer may have partial results appended.
 */
inline bool ValueToStringBuffer(JSContext* cx, const Value& v,
                                StringBuffer& sb);

} /* namespace js */

MOZ_ALWAYS_INLINE bool JSString::getChar(JSContext* cx, size_t index,
                                         char16_t* code) {
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

  if (!str->ensureLinear(cx)) {
    return false;
  }

  *code = str->asLinear().latin1OrTwoByteChar(index);
  return true;
}

MOZ_ALWAYS_INLINE JSLinearString* JSString::ensureLinear(JSContext* cx) {
  return isLinear() ? &asLinear() : asRope().flatten(cx);
}

inline JSLinearString* JSString::base() const {
  MOZ_ASSERT(hasBase());
  MOZ_ASSERT(!d.s.u3.base->isInline());
  return d.s.u3.base;
}

inline JSLinearString* JSString::nurseryBaseOrRelocOverlay() const {
  MOZ_ASSERT(hasBase());
  return d.s.u3.base;
}

inline bool JSString::canOwnDependentChars() const {
  // A string that could own the malloced chars used by another (dependent)
  // string. It will not have a base and must be linear and non-inline.
  return isLinear() && !isInline() && !hasBase();
}

inline void JSString::setBase(JSLinearString* newBase) {
  MOZ_ASSERT(hasBase());
  MOZ_ASSERT(!newBase->isInline());
  d.s.u3.base = newBase;
}

template <>
MOZ_ALWAYS_INLINE const char16_t* JSLinearString::nonInlineChars(
    const JS::AutoRequireNoGC& nogc) const {
  return nonInlineTwoByteChars(nogc);
}

template <>
MOZ_ALWAYS_INLINE const JS::Latin1Char* JSLinearString::nonInlineChars(
    const JS::AutoRequireNoGC& nogc) const {
  return nonInlineLatin1Chars(nogc);
}

template <>
MOZ_ALWAYS_INLINE const char16_t* JSLinearString::chars(
    const JS::AutoRequireNoGC& nogc) const {
  return rawTwoByteChars();
}

template <>
MOZ_ALWAYS_INLINE const JS::Latin1Char* JSLinearString::chars(
    const JS::AutoRequireNoGC& nogc) const {
  return rawLatin1Chars();
}

template <>
MOZ_ALWAYS_INLINE js::UniquePtr<JS::Latin1Char[], JS::FreePolicy>
JSRope::copyChars<JS::Latin1Char>(JSContext* maybecx,
                                  arena_id_t destArenaId) const {
  return copyLatin1Chars(maybecx, destArenaId);
}

template <>
MOZ_ALWAYS_INLINE JS::UniqueTwoByteChars JSRope::copyChars<char16_t>(
    JSContext* maybecx, arena_id_t destArenaId) const {
  return copyTwoByteChars(maybecx, destArenaId);
}

template <>
MOZ_ALWAYS_INLINE bool JSThinInlineString::lengthFits<JS::Latin1Char>(
    size_t length) {
  return length <= MAX_LENGTH_LATIN1;
}

template <>
MOZ_ALWAYS_INLINE bool JSThinInlineString::lengthFits<char16_t>(size_t length) {
  return length <= MAX_LENGTH_TWO_BYTE;
}

template <>
MOZ_ALWAYS_INLINE bool JSFatInlineString::lengthFits<JS::Latin1Char>(
    size_t length) {
  static_assert(
      (INLINE_EXTENSION_CHARS_LATIN1 * sizeof(char)) % js::gc::CellAlignBytes ==
          0,
      "fat inline strings' Latin1 characters don't exactly "
      "fill subsequent cells and thus are wasteful");
  static_assert(MAX_LENGTH_LATIN1 ==
                    (sizeof(JSFatInlineString) -
                     offsetof(JSFatInlineString, d.inlineStorageLatin1)) /
                        sizeof(char),
                "MAX_LENGTH_LATIN1 must be one less than inline Latin1 "
                "storage count");

  return length <= MAX_LENGTH_LATIN1;
}

template <>
MOZ_ALWAYS_INLINE bool JSFatInlineString::lengthFits<char16_t>(size_t length) {
  static_assert((INLINE_EXTENSION_CHARS_TWO_BYTE * sizeof(char16_t)) %
                        js::gc::CellAlignBytes ==
                    0,
                "fat inline strings' char16_t characters don't exactly "
                "fill subsequent cells and thus are wasteful");
  static_assert(MAX_LENGTH_TWO_BYTE ==
                    (sizeof(JSFatInlineString) -
                     offsetof(JSFatInlineString, d.inlineStorageTwoByte)) /
                        sizeof(char16_t),
                "MAX_LENGTH_TWO_BYTE must be one less than inline "
                "char16_t storage count");

  return length <= MAX_LENGTH_TWO_BYTE;
}

template <>
MOZ_ALWAYS_INLINE bool JSInlineString::lengthFits<JS::Latin1Char>(
    size_t length) {
  // If it fits in a fat inline string, it fits in any inline string.
  return JSFatInlineString::lengthFits<JS::Latin1Char>(length);
}

template <>
MOZ_ALWAYS_INLINE bool JSInlineString::lengthFits<char16_t>(size_t length) {
  // If it fits in a fat inline string, it fits in any inline string.
  return JSFatInlineString::lengthFits<char16_t>(length);
}

template <>
MOZ_ALWAYS_INLINE void JSString::setNonInlineChars(const char16_t* chars) {
  // Check that the new buffer is located in the StringBufferArena
  checkStringCharsArena(chars);
  d.s.u2.nonInlineCharsTwoByte = chars;
}

template <>
MOZ_ALWAYS_INLINE void JSString::setNonInlineChars(
    const JS::Latin1Char* chars) {
  // Check that the new buffer is located in the StringBufferArena
  checkStringCharsArena(chars);
  d.s.u2.nonInlineCharsLatin1 = chars;
}

MOZ_ALWAYS_INLINE const JS::Latin1Char* JSLinearString::rawLatin1Chars() const {
  MOZ_ASSERT(JSString::isLinear());
  MOZ_ASSERT(hasLatin1Chars());
  return isInline() ? d.inlineStorageLatin1 : d.s.u2.nonInlineCharsLatin1;
}

MOZ_ALWAYS_INLINE const char16_t* JSLinearString::rawTwoByteChars() const {
  MOZ_ASSERT(JSString::isLinear());
  MOZ_ASSERT(hasTwoByteChars());
  return isInline() ? d.inlineStorageTwoByte : d.s.u2.nonInlineCharsTwoByte;
}

inline js::PropertyName* JSAtom::asPropertyName() {
  MOZ_ASSERT(!isIndex());
  return static_cast<js::PropertyName*>(this);
}

inline bool JSLinearString::isIndex(uint32_t* indexp) const {
  MOZ_ASSERT(JSString::isLinear());

  if (isAtom()) {
    return asAtom().isIndex(indexp);
  }

  if (JSString::hasIndexValue()) {
    *indexp = getIndexValue();
    return true;
  }

  return isIndexSlow(indexp);
}

inline size_t JSLinearString::allocSize() const {
  MOZ_ASSERT(ownsMallocedChars());

  size_t charSize =
      hasLatin1Chars() ? sizeof(JS::Latin1Char) : sizeof(char16_t);
  size_t count = isExtensible() ? asExtensible().capacity() : length();
  return count * charSize;
}

inline size_t JSString::allocSize() const {
  return ownsMallocedChars() ? asLinear().allocSize() : 0;
}

namespace js {
namespace gc {
template <>
inline JSString* Cell::as<JSString>() {
  MOZ_ASSERT(is<JSString>());
  return reinterpret_cast<JSString*>(this);
}

template <>
inline JSString* TenuredCell::as<JSString>() {
  MOZ_ASSERT(is<JSString>());
  return reinterpret_cast<JSString*>(this);
}

// StringRelocationOverlay assists with updating the string chars
// pointers of dependent strings when their base strings are
// deduplicated. It stores:
//  - nursery chars of a root base (root base is a non-dependent base), or
//  - nursery base of a dependent string
// StringRelocationOverlay exploits the fact that the 3rd word of a JSString's
// RelocationOverlay is not utilized and can be used to store extra information.
class StringRelocationOverlay : public RelocationOverlay {
  union {
    // nursery chars of a root base
    const JS::Latin1Char* nurseryCharsLatin1;
    const char16_t* nurseryCharsTwoByte;

    // The nursery base can be forwarded, which becomes a string relocation
    // overlay, or it is not yet forwarded and is simply the base.
    JSLinearString* nurseryBaseOrRelocOverlay;
  };

 public:
  explicit StringRelocationOverlay(Cell* dst) : RelocationOverlay(dst) {
    static_assert(sizeof(JSString) >= sizeof(StringRelocationOverlay));
  }

  static const StringRelocationOverlay* fromCell(const Cell* cell) {
    return static_cast<const StringRelocationOverlay*>(cell);
  }

  static StringRelocationOverlay* fromCell(Cell* cell) {
    return static_cast<StringRelocationOverlay*>(cell);
  }

  StringRelocationOverlay*& nextRef() {
    MOZ_ASSERT(isForwarded());
    return (StringRelocationOverlay*&)next_;
  }

  StringRelocationOverlay* next() const {
    MOZ_ASSERT(isForwarded());
    return (StringRelocationOverlay*)next_;
  }

  template <typename CharT>
  MOZ_ALWAYS_INLINE const CharT* savedNurseryChars() const;

  const MOZ_ALWAYS_INLINE JS::Latin1Char* savedNurseryCharsLatin1() const {
    return nurseryCharsLatin1;
  }

  const MOZ_ALWAYS_INLINE char16_t* savedNurseryCharsTwoByte() const {
    return nurseryCharsTwoByte;
  }

  JSLinearString* savedNurseryBaseOrRelocOverlay() const {
    return nurseryBaseOrRelocOverlay;
  }

  // Transform a nursery string to a StringRelocationOverlay that is forwarded
  // to a tenured string.
  inline static StringRelocationOverlay* forwardCell(JSString* src, Cell* dst) {
    MOZ_ASSERT(!src->isForwarded());
    MOZ_ASSERT(!dst->isForwarded());

    JS::AutoCheckCannotGC nogc;
    StringRelocationOverlay* overlay;

    // Initialize the overlay, and remember the nursery base string if there is
    // one, or nursery non-inlined chars if it can be the root base of other
    // strings.
    //
    // The non-inlined chars of a tenured dependent string should point to the
    // tenured root base's one with an offset. For example, a dependent string
    // may start from the 3rd char of its root base. During tenuring, offsets
    // of dependent strings can be computed from the nursery non-inlined chars
    // remembered in overlays.
    if (src->hasBase()) {
      auto nurseryBaseOrRelocOverlay = src->nurseryBaseOrRelocOverlay();
      overlay = new (src) StringRelocationOverlay(dst);
      overlay->nurseryBaseOrRelocOverlay = nurseryBaseOrRelocOverlay;
    } else if (src->canOwnDependentChars()) {
      if (src->hasTwoByteChars()) {
        auto nurseryCharsTwoByte = src->asLinear().twoByteChars(nogc);
        overlay = new (src) StringRelocationOverlay(dst);
        overlay->nurseryCharsTwoByte = nurseryCharsTwoByte;
      } else {
        auto nurseryCharsLatin1 = src->asLinear().latin1Chars(nogc);
        overlay = new (src) StringRelocationOverlay(dst);
        overlay->nurseryCharsLatin1 = nurseryCharsLatin1;
      }
    } else {
      overlay = new (src) StringRelocationOverlay(dst);
    }

    return overlay;
  }
};

template <>
MOZ_ALWAYS_INLINE const JS::Latin1Char*
StringRelocationOverlay::savedNurseryChars() const {
  return savedNurseryCharsLatin1();
}

template <>
MOZ_ALWAYS_INLINE const char16_t* StringRelocationOverlay::savedNurseryChars()
    const {
  return savedNurseryCharsTwoByte();
}

}  // namespace gc
}  // namespace js

#endif /* vm_StringType_h */
