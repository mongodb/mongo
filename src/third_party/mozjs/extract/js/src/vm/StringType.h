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
#include "mozilla/Span.h"
#include "mozilla/TextUtils.h"

#include <string_view>  // std::basic_string_view

#include "jstypes.h"  // js::Bit

#include "gc/Cell.h"
#include "gc/MaybeRooted.h"
#include "gc/Nursery.h"
#include "gc/RelocationOverlay.h"
#include "gc/StoreBuffer.h"
#include "js/CharacterEncoding.h"
#include "js/RootingAPI.h"
#include "js/shadow/String.h"  // JS::shadow::String
#include "js/String.h"         // JS::MaxStringLength
#include "js/UniquePtr.h"
#include "util/Text.h"

class JSDependentString;
class JSExtensibleString;
class JSExternalString;
class JSInlineString;
class JSRope;

namespace JS {
class JS_PUBLIC_API AutoStableStringChars;
}  // namespace JS

namespace js {

class ArrayObject;
class JS_PUBLIC_API GenericPrinter;
class JSONPrinter;
class PropertyName;
class StringBuffer;

namespace frontend {
class ParserAtomsTable;
class TaggedParserAtomIndex;
class WellKnownParserAtoms;
struct CompilationAtomCache;
}  // namespace frontend

namespace jit {
class MacroAssembler;
}  // namespace jit

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
 * JSLinearString               latin1Chars, twoByteChars / -
 *  |
 *  +-- JSDependentString       base / -
 *  |   |
 *  |   +-- JSAtomRefString     - / base points to an atom
 *  |
 *  +-- JSExternalString        - / char array memory managed by embedding
 *  |
 *  +-- JSExtensibleString      - / tracks total buffer capacity (including current text)
 *  |
 *  +-- JSInlineString (abstract) - / chars stored in header
 *  |   |
 *  |   +-- JSThinInlineString  - / header is normal
 *  |   |
 *  |   +-- JSFatInlineString   - / header is fat
 *  |
 * JSAtom (abstract)            - / string equality === pointer equality
 *  |  |
 *  |  +-- js::NormalAtom       JSLinearString + atom hash code / -
 *  |  |   |
 *  |  |   +-- js::ThinInlineAtom
 *  |  |                        possibly larger JSThinInlineString + atom hash code / -
 *  |  |
 *  |  +-- js::FatInlineAtom    JSFatInlineString w/atom hash code / -
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
 * The ensureX() operations mutate 'this' in place to effectively make the type
 * be at least X (e.g., ensureLinear will change a JSRope to be a JSLinearString).
 */
// clang-format on

class JSString : public js::gc::CellWithLengthAndFlags {
 protected:
  using Base = js::gc::CellWithLengthAndFlags;

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

  // Class for temporarily holding character data that will be used for JSString
  // contents. The data may be allocated in the nursery, the malloc heap, or in
  // externally owned memory (perhaps on the stack). The class instance must be
  // passed to the JSString constructor as a MutableHandle, so that if a GC
  // occurs between the construction of the content and the construction of the
  // JSString Cell to hold it, the contents can be transparently moved to the
  // malloc heap before the nursery is reset.
  template <typename CharT>
  class OwnedChars {
    mozilla::Span<CharT> chars_;
    bool needsFree_;
    bool isMalloced_;

   public:
    // needsFree: the chars pointer should be passed to js_free() if OwnedChars
    // dies while still possessing ownership.
    //
    // isMalloced: the chars pointer does not point into the nursery.
    //
    // These are not quite the same, since you might have non-nursery characters
    // that are owned by something else. needsFree implies isMalloced.
    OwnedChars(CharT* chars, size_t length, bool isMalloced, bool needsFree);
    OwnedChars(js::UniquePtr<CharT[], JS::FreePolicy>&& chars, size_t length,
               bool isMalloced);
    OwnedChars(OwnedChars&&);
    OwnedChars(const OwnedChars&) = delete;
    ~OwnedChars() { reset(); }

    explicit operator bool() const { return !chars_.empty(); }
    mozilla::Span<CharT> span() const { return chars_; }
    CharT* data() const { return chars_.data(); }
    size_t length() const { return chars_.Length(); }
    size_t size() const { return length() * sizeof(CharT); }
    bool isMalloced() const { return isMalloced_; }

    // Return the data and release ownership to the caller.
    inline CharT* release();
    // Discard any owned data.
    inline void reset();
    // Move any nursery data into the malloc heap.
    inline void ensureNonNursery();

    // If we GC with a live OwnedChars, copy the data out of the nursery to a
    // safely malloced location.
    void trace(JSTracer* trc) { ensureNonNursery(); }
  };

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
          JSAtom* atom;         /* JSAtomRefString */
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
   * string in this case. When we flatten a TwoByte rope, we turn child ropes
   * (including Latin1 ropes) into TwoByte dependent strings. If one of these
   * strings is also part of another Latin1 rope tree, we can have a Latin1 rope
   * with a TwoByte descendent.
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
   *   String         Instance        Subtype
   *   type           encoding        predicate
   *   -----------------------------------------
   *   Rope           0000000 000     xxxxx0x xxx
   *   Linear         0000010 000     xxxxx1x xxx
   *   Dependent      0000110 000     xxxx1xx xxx
   *   AtomRef        1000110 000     1xxxxxx xxx
   *   External       0100010 000     x100010 xxx
   *   Extensible     0010010 000     x010010 xxx
   *   Inline         0001010 000     xxx1xxx xxx
   *   FatInline      0011010 000     xx11xxx xxx
   *   JSAtom         -               xxxxxx1 xxx
   *   NormalAtom     0000011 000     xxx0xx1 xxx
   *   PermanentAtom  0100011 000     x1xxxx1 xxx
   *   ThinInlineAtom 0001011 000     xx01xx1 xxx
   *   FatInlineAtom  0011011 000     xx11xx1 xxx
   *                                  ||||||| |||
   *                                  ||||||| ||\- [0] reserved (FORWARD_BIT)
   *                                  ||||||| |\-- [1] reserved
   *                                  ||||||| \--- [2] reserved
   *                                  ||||||\----- [3] IsAtom
   *                                  |||||\------ [4] IsLinear
   *                                  ||||\------- [5] IsDependent
   *                                  |||\-------- [6] IsInline
   *                                  ||\--------- [7] FatInlineAtom/Extensible
   *                                  |\---------- [8] External/Permanent
   *                                  \----------- [9] AtomRef
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
   *   Bit 6: IsInline (Inline, FatInline, ThinInlineAtom, FatInlineAtom)
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
  // Indicates a dependent string pointing to an atom
  static const uint32_t ATOM_REF_BIT = js::Bit(9);

  static const uint32_t LINEAR_IS_EXTENSIBLE_BIT = js::Bit(7);
  static const uint32_t INLINE_IS_FAT_BIT = js::Bit(7);

  static const uint32_t LINEAR_IS_EXTERNAL_BIT = js::Bit(8);
  static const uint32_t ATOM_IS_PERMANENT_BIT = js::Bit(8);

  static const uint32_t EXTENSIBLE_FLAGS =
      LINEAR_BIT | LINEAR_IS_EXTENSIBLE_BIT;
  static const uint32_t EXTERNAL_FLAGS = LINEAR_BIT | LINEAR_IS_EXTERNAL_BIT;

  static const uint32_t FAT_INLINE_MASK = INLINE_CHARS_BIT | INLINE_IS_FAT_BIT;

  /* Initial flags for various types of strings. */
  static const uint32_t INIT_THIN_INLINE_FLAGS = LINEAR_BIT | INLINE_CHARS_BIT;
  static const uint32_t INIT_FAT_INLINE_FLAGS = LINEAR_BIT | FAT_INLINE_MASK;
  static const uint32_t INIT_ROPE_FLAGS = 0;
  static const uint32_t INIT_LINEAR_FLAGS = LINEAR_BIT;
  static const uint32_t INIT_DEPENDENT_FLAGS = LINEAR_BIT | DEPENDENT_BIT;
  static const uint32_t INIT_ATOM_REF_FLAGS =
      INIT_DEPENDENT_FLAGS | ATOM_REF_BIT;

  static const uint32_t TYPE_FLAGS_MASK = js::BitMask(10) - js::BitMask(3);
  static_assert((TYPE_FLAGS_MASK & js::gc::HeaderWord::RESERVED_MASK) == 0,
                "GC reserved bits must not be used for Strings");

  // Whether this atom's characters store an uint32 index value less than or
  // equal to MAX_ARRAY_INDEX. This bit means something different if the
  // string is not an atom (see ATOM_REF_BIT)
  // See JSLinearString::isIndex.
  static const uint32_t ATOM_IS_INDEX_BIT = js::Bit(9);

  // Linear strings:
  // - Content and representation are Latin-1 characters.
  // - Unmodifiable after construction.
  //
  // Ropes:
  // - Content are Latin-1 characters.
  // - Flag may be cleared when the rope is changed into a dependent string.
  //
  // Also see LATIN1_CHARS_BIT description under "Flag Encoding".
  static const uint32_t LATIN1_CHARS_BIT = js::Bit(10);

  static const uint32_t INDEX_VALUE_BIT = js::Bit(11);
  static const uint32_t INDEX_VALUE_SHIFT = 16;

  // NON_DEDUP_BIT is used in string deduplication during tenuring. This bit is
  // shared with both FLATTEN_FINISH_NODE and ATOM_IS_PERMANENT_BIT, since it
  // only applies to linear non-atoms.
  static const uint32_t NON_DEDUP_BIT = js::Bit(15);

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

  // Indicates that this string is depended on by another string. A rope should
  // never be depended on, and this should never be set during flattening, so
  // we can reuse the FLATTEN_VISIT_RIGHT bit.
  static const uint32_t DEPENDED_ON_BIT = FLATTEN_VISIT_RIGHT;

  static const uint32_t PINNED_ATOM_BIT = js::Bit(15);
  static const uint32_t PERMANENT_ATOM_MASK =
      ATOM_BIT | PINNED_ATOM_BIT | ATOM_IS_PERMANENT_BIT;

  static const uint32_t MAX_LENGTH = JS::MaxStringLength;

  static const JS::Latin1Char MAX_LATIN1_CHAR = 0xff;

  /*
   * Helper function to validate that a string of a given length is
   * representable by a JSString. An allocation overflow is reported if false
   * is returned.
   */
  static inline bool validateLength(JSContext* maybecx, size_t length);

  template <js::AllowGC allowGC>
  static inline bool validateLengthInternal(JSContext* maybecx, size_t length);

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
  inline bool getCodePoint(JSContext* cx, size_t index, char32_t* codePoint);

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

  /*
   * Whether any dependent strings point to this string's chars. This is needed
   * so that we don't replace the string with a forwarded atom and free its
   * buffer.
   *
   * NOTE: we specifically do not set this for atoms, because they are accessed
   * on many threads and we don't want to mess with their flags if we don't
   * have to, and it is safe because atoms will never be replaced by an atom
   * ref.
   */
  bool isDependedOn() const {
    bool result = flags() & DEPENDED_ON_BIT;
    MOZ_ASSERT_IF(result, !isRope() && !isAtom());
    return result;
  }

  bool assertIsValidBase() const {
    // See isDependedOn comment for why we're excluding atoms
    return isAtom() || isDependedOn();
  }

  void setDependedOn() {
    MOZ_ASSERT(!isRope());
    if (isAtom()) {
      return;
    }
    setFlagBit(DEPENDED_ON_BIT);
  }

  inline size_t allocSize() const;

  /* Fallible conversions to more-derived string types. */

  inline JSLinearString* ensureLinear(JSContext* cx);

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
  bool isAtomRef() const {
    return (flags() & ATOM_REF_BIT) && !(flags() & ATOM_BIT);
  }

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
  void setNonDeduplicatable() {
    MOZ_ASSERT(isLinear());
    MOZ_ASSERT(!isAtom());
    setFlagBit(NON_DEDUP_BIT);
  }

  // After copying a string from the nursery to the tenured heap, adjust bits
  // that no longer apply.
  MOZ_ALWAYS_INLINE
  void clearBitsOnTenure() {
    MOZ_ASSERT(!isAtom());
    clearFlagBit(NON_DEDUP_BIT | IN_STRING_TO_ATOM_CACHE);
  }

  // NON_DEDUP_BIT is only valid for linear non-atoms.
  MOZ_ALWAYS_INLINE
  bool isDeduplicatable() {
    MOZ_ASSERT(isLinear());
    MOZ_ASSERT(!isAtom());
    return !(flags() & NON_DEDUP_BIT);
  }

  void setInStringToAtomCache() {
    MOZ_ASSERT(!isAtom());
    setFlagBit(IN_STRING_TO_ATOM_CACHE);
  }
  bool inStringToAtomCache() const { return flags() & IN_STRING_TO_ATOM_CACHE; }

  // Fills |array| with various strings that represent the different string
  // kinds and character encodings.
  static bool fillWithRepresentatives(JSContext* cx,
                                      JS::Handle<js::ArrayObject*> array);

  /* Only called by the GC for dependent strings. */

  inline bool hasBase() const { return isDependent(); }

  inline JSLinearString* base() const;

  inline JSAtom* atom() const;

  // The base may be forwarded and becomes a relocation overlay.
  // The return value can be a relocation overlay when the base is forwarded,
  // or the return value can be the actual base when it is not forwarded.
  inline JSLinearString* nurseryBaseOrRelocOverlay() const;

  inline bool canOwnDependentChars() const;

  // Only called by the GC during nursery collection.
  inline void setBase(JSLinearString* newBase);

  bool tryReplaceWithAtomRef(JSAtom* atom);

  void traceBase(JSTracer* trc);

  /* Only called by the GC for strings with the AllocKind::STRING kind. */

  inline void finalize(JS::GCContext* gcx);

  /* Gets the number of bytes that the chars take on the heap. */

  size_t sizeOfExcludingThis(mozilla::MallocSizeOf mallocSizeOf);

  bool hasOutOfLineChars() const {
    return isLinear() && !isInline() && !isDependent() && !isExternal();
  }

  inline bool ownsMallocedChars() const;

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
  mozilla::Maybe<std::tuple<size_t, size_t>> encodeUTF8Partial(
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

#if defined(DEBUG) || defined(JS_JITSPEW) || defined(JS_CACHEIR_SPEW)
  void dump() const;
  void dump(js::GenericPrinter& out) const;
  void dump(js::JSONPrinter& json) const;

  void dumpCommonFields(js::JSONPrinter& json) const;
  void dumpCharsFields(js::JSONPrinter& json) const;

  void dumpFields(js::JSONPrinter& json) const;
  void dumpStringContent(js::GenericPrinter& out) const;
  void dumpPropertyName(js::GenericPrinter& out) const;

  void dumpChars(js::GenericPrinter& out) const;
  void dumpCharsSingleQuote(js::GenericPrinter& out) const;
  void dumpCharsNoQuote(js::GenericPrinter& out) const;

  template <typename CharT>
  static void dumpCharsNoQuote(const CharT* s, size_t len,
                               js::GenericPrinter& out);

  void dumpRepresentation() const;
  void dumpRepresentation(js::GenericPrinter& out) const;
  void dumpRepresentation(js::JSONPrinter& json) const;
  void dumpRepresentationFields(js::JSONPrinter& json) const;

  bool equals(const char* s);
#endif

  void traceChildren(JSTracer* trc);

  inline void traceBaseFromStoreBuffer(JSTracer* trc);

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
  JSString(const JSString& other) = delete;
  void operator=(const JSString& other) = delete;

 protected:
  JSString() = default;
};

namespace js {

template <typename Wrapper, typename CharT>
class WrappedPtrOperations<JSString::OwnedChars<CharT>, Wrapper> {
  const JSString::OwnedChars<CharT>& get() const {
    return static_cast<const Wrapper*>(this)->get();
  }

 public:
  explicit operator bool() const { return !!get(); }
  mozilla::Span<CharT> span() const { return get().span(); }
  CharT* data() const { return get().data(); }
  size_t length() const { return get().length(); }
  size_t size() const { return get().size(); }
  bool isMalloced() const { return get().isMalloced(); }
};

template <typename Wrapper, typename CharT>
class MutableWrappedPtrOperations<JSString::OwnedChars<CharT>, Wrapper>
    : public WrappedPtrOperations<JSString::OwnedChars<CharT>, Wrapper> {
  JSString::OwnedChars<CharT>& get() {
    return static_cast<Wrapper*>(this)->get();
  }

 public:
  CharT* release() { return get().release(); }
  void reset() { get().reset(); }
  void ensureNonNursery() { get().ensureNonNursery(); }
};

} /* namespace js */

class JSRope : public JSString {
  friend class js::gc::CellAllocator;

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

  JSRope(JSString* left, JSString* right, size_t length);

 public:
  template <js::AllowGC allowGC>
  static inline JSRope* new_(
      JSContext* cx,
      typename js::MaybeRooted<JSString*, allowGC>::HandleType left,
      typename js::MaybeRooted<JSString*, allowGC>::HandleType right,
      size_t length, js::gc::Heap = js::gc::Heap::Default);

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

#if defined(DEBUG) || defined(JS_JITSPEW) || defined(JS_CACHEIR_SPEW)
  void dumpOwnRepresentationFields(js::JSONPrinter& json) const;
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

/*
 * There are optimized entry points for some string allocation functions.
 *
 * The meaning of suffix:
 *   * "MaybeDeflate": for char16_t variant, characters can fit Latin1
 *   * "DontDeflate": for char16_t variant, characters don't fit Latin1
 *   * "NonStatic": characters don't match StaticStrings
 *   * "ValidLength": length fits JSString::MAX_LENGTH
 */

class JSLinearString : public JSString {
  friend class JSString;
  friend class JS::AutoStableStringChars;
  friend class js::gc::TenuringTracer;
  friend class js::gc::CellAllocator;
  friend class JSDependentString;  // To allow access when used as base.

  /* Vacuous and therefore unimplemented. */
  JSLinearString* ensureLinear(JSContext* cx) = delete;
  bool isLinear() const = delete;
  JSLinearString& asLinear() const = delete;

  JSLinearString(const char16_t* chars, size_t length);
  JSLinearString(const JS::Latin1Char* chars, size_t length);
  template <typename CharT>
  explicit inline JSLinearString(JS::MutableHandle<OwnedChars<CharT>> chars);

 protected:
  // Used to construct subclasses that do a full initialization themselves.
  JSLinearString() = default;

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
  template <js::AllowGC allowGC, typename CharT>
  static inline JSLinearString* new_(JSContext* cx,
                                     JS::MutableHandle<OwnedChars<CharT>> chars,
                                     js::gc::Heap heap);

  template <js::AllowGC allowGC, typename CharT>
  static inline JSLinearString* newValidLength(
      JSContext* cx, JS::MutableHandle<OwnedChars<CharT>> chars,
      js::gc::Heap heap);

  // Convert a plain linear string to an extensible string. For testing. The
  // caller must ensure that it is a plain or extensible string already, and
  // that `capacity` is adequate.
  JSExtensibleString& makeExtensible(size_t capacity);

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

  // Return whether the characters of this string can be moved by minor or
  // compacting GC.
  inline bool hasMovableChars() const;

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

  // Make sure chars are not in the nursery, mallocing and copying if necessary.
  // Should only be called during minor GC on a string that has been promoted
  // to the tenured heap and may still point to nursery-allocated chars.
  template <typename CharT>
  inline size_t maybeMallocCharsOnPromotion(js::Nursery* nursery);

  inline void finalize(JS::GCContext* gcx);
  inline size_t allocSize() const;

#if defined(DEBUG) || defined(JS_JITSPEW) || defined(JS_CACHEIR_SPEW)
  void dumpOwnRepresentationFields(js::JSONPrinter& json) const;
#endif

  // Make a partially-initialized string safe for finalization.
  inline void disownCharsBecauseError();
};

static_assert(sizeof(JSLinearString) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

class JSDependentString : public JSLinearString {
  friend class JSString;
  friend class js::gc::CellAllocator;

  JSDependentString(JSLinearString* base, size_t start, size_t length);

  // For JIT string allocation.
  JSDependentString() = default;

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
  // This will always return a dependent string, and will assert if the chars
  // could fit into an inline string.
  static inline JSLinearString* new_(JSContext* cx, JSLinearString* base,
                                     size_t start, size_t length,
                                     js::gc::Heap heap);

  template <typename T>
  void relocateNonInlineChars(T chars, size_t offset) {
    setNonInlineChars(chars + offset);
  }

  inline JSLinearString* rootBaseDuringMinorGC();

  template <typename CharT>
  inline void sweepTypedAfterMinorGC();

  inline void sweepAfterMinorGC();

#if defined(DEBUG) || defined(JS_JITSPEW) || defined(JS_CACHEIR_SPEW)
  void dumpOwnRepresentationFields(js::JSONPrinter& json) const;
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

class JSAtomRefString : public JSDependentString {
  friend class JSString;
  friend class js::gc::CellAllocator;
  friend class js::jit::MacroAssembler;

 public:
  inline static size_t offsetOfAtom() {
    return offsetof(JSAtomRefString, d.s.u3.atom);
  }
};

static_assert(sizeof(JSAtomRefString) == sizeof(JSString),
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

#if defined(DEBUG) || defined(JS_JITSPEW) || defined(JS_CACHEIR_SPEW)
  void dumpOwnRepresentationFields(js::JSONPrinter& json) const;
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

#if defined(DEBUG) || defined(JS_JITSPEW) || defined(JS_CACHEIR_SPEW)
  void dumpOwnRepresentationFields(js::JSONPrinter& json) const;
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
  friend class js::gc::CellAllocator;

  // The constructors return a mutable pointer to the data, because the first
  // thing any creator will do is copy in the string value. This also
  // conveniently allows doing overload resolution on CharT.
  explicit JSThinInlineString(size_t length, JS::Latin1Char** chars);
  explicit JSThinInlineString(size_t length, char16_t** chars);

  // For JIT string allocation.
  JSThinInlineString() = default;

 public:
  static constexpr size_t InlineBytes = NUM_INLINE_CHARS_LATIN1;

  static const size_t MAX_LENGTH_LATIN1 = NUM_INLINE_CHARS_LATIN1;
  static const size_t MAX_LENGTH_TWO_BYTE = NUM_INLINE_CHARS_TWO_BYTE;

  template <js::AllowGC allowGC>
  static inline JSThinInlineString* new_(JSContext* cx, js::gc::Heap heap);

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
  friend class js::gc::CellAllocator;

  static const size_t INLINE_EXTENSION_CHARS_LATIN1 =
      24 - NUM_INLINE_CHARS_LATIN1;
  static const size_t INLINE_EXTENSION_CHARS_TWO_BYTE =
      12 - NUM_INLINE_CHARS_TWO_BYTE;

  // The constructors return a mutable pointer to the data, because the first
  // thing any creator will do is copy in the string value. This also
  // conveniently allows doing overload resolution on CharT.
  explicit JSFatInlineString(size_t length, JS::Latin1Char** chars);
  explicit JSFatInlineString(size_t length, char16_t** chars);

  // For JIT string allocation.
  JSFatInlineString() = default;

 protected: /* to fool clang into not warning this is unused */
  union {
    char inlineStorageExtensionLatin1[INLINE_EXTENSION_CHARS_LATIN1];
    char16_t inlineStorageExtensionTwoByte[INLINE_EXTENSION_CHARS_TWO_BYTE];
  };

 public:
  template <js::AllowGC allowGC>
  static inline JSFatInlineString* new_(JSContext* cx, js::gc::Heap heap);

  static const size_t MAX_LENGTH_LATIN1 =
      JSString::NUM_INLINE_CHARS_LATIN1 + INLINE_EXTENSION_CHARS_LATIN1;

  static const size_t MAX_LENGTH_TWO_BYTE =
      JSString::NUM_INLINE_CHARS_TWO_BYTE + INLINE_EXTENSION_CHARS_TWO_BYTE;

  template <typename CharT>
  static bool lengthFits(size_t length);

  // Only called by the GC for strings with the AllocKind::FAT_INLINE_STRING
  // kind.
  MOZ_ALWAYS_INLINE void finalize(JS::GCContext* gcx);
};

static_assert(sizeof(JSFatInlineString) % js::gc::CellAlignBytes == 0,
              "fat inline strings shouldn't waste space up to the next cell "
              "boundary");

class JSExternalString : public JSLinearString {
  friend class js::gc::CellAllocator;

  JSExternalString(const JS::Latin1Char* chars, size_t length,
                   const JSExternalStringCallbacks* callbacks);
  JSExternalString(const char16_t* chars, size_t length,
                   const JSExternalStringCallbacks* callbacks);

  /* Vacuous and therefore unimplemented. */
  bool isExternal() const = delete;
  JSExternalString& asExternal() const = delete;

  template <typename CharT>
  static inline JSExternalString* newImpl(
      JSContext* cx, const CharT* chars, size_t length,
      const JSExternalStringCallbacks* callbacks);

 public:
  static inline JSExternalString* new_(
      JSContext* cx, const JS::Latin1Char* chars, size_t length,
      const JSExternalStringCallbacks* callbacks);
  static inline JSExternalString* new_(
      JSContext* cx, const char16_t* chars, size_t length,
      const JSExternalStringCallbacks* callbacks);

  const JSExternalStringCallbacks* callbacks() const {
    MOZ_ASSERT(JSString::isExternal());
    return d.s.u3.externalCallbacks;
  }

  // External chars are never allocated inline or in the nursery, so we can
  // safely expose this without requiring an AutoCheckCannotGC argument.
  const JS::Latin1Char* latin1Chars() const { return rawLatin1Chars(); }
  const char16_t* twoByteChars() const { return rawTwoByteChars(); }

  // Only called by the GC for strings with the AllocKind::EXTERNAL_STRING
  // kind.
  inline void finalize(JS::GCContext* gcx);

#if defined(DEBUG) || defined(JS_JITSPEW) || defined(JS_CACHEIR_SPEW)
  void dumpOwnRepresentationFields(js::JSONPrinter& json) const;
#endif
};

static_assert(sizeof(JSExternalString) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

class JSAtom : public JSLinearString {
  /* Vacuous and therefore unimplemented. */
  bool isAtom() const = delete;
  JSAtom& asAtom() const = delete;

 public:
  template <typename CharT>
  static inline JSAtom* newValidLength(
      JSContext* cx, js::UniquePtr<CharT[], JS::FreePolicy> chars,
      size_t length, js::HashNumber hash);

  /* Returns the PropertyName for this.  isIndex() must be false. */
  inline js::PropertyName* asPropertyName();

  MOZ_ALWAYS_INLINE
  bool isPermanent() const { return JSString::isPermanentAtom(); }

  MOZ_ALWAYS_INLINE
  void makePermanent() {
    MOZ_ASSERT(JSString::isAtom());
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

  MOZ_ALWAYS_INLINE bool isPinned() const { return flags() & PINNED_ATOM_BIT; }

  void setPinned() {
    MOZ_ASSERT(!isPinned());
    setFlagBit(PINNED_ATOM_BIT);
  }

  inline js::HashNumber hash() const;
  inline void initHash(js::HashNumber hash);

  template <typename CharT>
  static bool lengthFitsInline(size_t length);

#if defined(DEBUG) || defined(JS_JITSPEW) || defined(JS_CACHEIR_SPEW)
  void dump(js::GenericPrinter& out);
  void dump();
#endif
};

namespace js {

class NormalAtom : public JSAtom {
  friend class gc::CellAllocator;

 protected:
  static constexpr size_t ExtensionBytes =
      js::gc::CellAlignBytes - sizeof(js::HashNumber);

  char inlineStorage_[ExtensionBytes];
  HashNumber hash_;

  // For subclasses to call.
  explicit NormalAtom(js::HashNumber hash) : hash_(hash) {}

  // Out of line atoms, mimicking JSLinearString constructors.
  NormalAtom(const char16_t* chars, size_t length, js::HashNumber hash);
  NormalAtom(const JS::Latin1Char* chars, size_t length, js::HashNumber hash);

 public:
  HashNumber hash() const { return hash_; }
  void initHash(HashNumber hash) { hash_ = hash; }

  static constexpr size_t offsetOfHash() { return offsetof(NormalAtom, hash_); }
};

static_assert(sizeof(NormalAtom) ==
                  js::RoundUp(sizeof(JSString) + sizeof(js::HashNumber),
                              js::gc::CellAlignBytes),
              "NormalAtom must have size of a string + HashNumber, "
              "aligned to gc::CellAlignBytes");

class ThinInlineAtom : public NormalAtom {
  friend class gc::CellAllocator;

 public:
  static constexpr size_t MAX_LENGTH_LATIN1 =
      NUM_INLINE_CHARS_LATIN1 + ExtensionBytes / sizeof(JS::Latin1Char);
  static constexpr size_t MAX_LENGTH_TWO_BYTE =
      NUM_INLINE_CHARS_TWO_BYTE + ExtensionBytes / sizeof(char16_t);

#ifdef JS_64BIT
  // Fat and Thin inline atoms are the same size. Only use fat.
  static constexpr bool EverInstantiated = false;
#else
  static constexpr bool EverInstantiated = true;
#endif

 protected:
  // Mimicking JSThinInlineString constructors.
#ifdef JS_64BIT
  ThinInlineAtom(size_t length, JS::Latin1Char** chars,
                 js::HashNumber hash) = delete;
  ThinInlineAtom(size_t length, char16_t** chars, js::HashNumber hash) = delete;
#else
  ThinInlineAtom(size_t length, JS::Latin1Char** chars, js::HashNumber hash);
  ThinInlineAtom(size_t length, char16_t** chars, js::HashNumber hash);
#endif

 public:
  template <typename CharT>
  static bool lengthFits(size_t length) {
    if constexpr (sizeof(CharT) == sizeof(JS::Latin1Char)) {
      return length <= MAX_LENGTH_LATIN1;
    } else {
      return length <= MAX_LENGTH_TWO_BYTE;
    }
  }
};

// FatInlineAtom is basically a JSFatInlineString, except it has a hash value in
// the last word that reduces the inline char storage.
class FatInlineAtom : public JSAtom {
  friend class gc::CellAllocator;

  // The space available for storing inline characters. It's the same amount of
  // space as a JSFatInlineString, except we take the hash value out of it.
  static constexpr size_t InlineBytes = sizeof(JSFatInlineString) -
                                        sizeof(JSString::Base) -
                                        sizeof(js::HashNumber);

  static constexpr size_t ExtensionBytes =
      InlineBytes - JSThinInlineString::InlineBytes;

 public:
  static constexpr size_t MAX_LENGTH_LATIN1 =
      InlineBytes / sizeof(JS::Latin1Char);
  static constexpr size_t MAX_LENGTH_TWO_BYTE = InlineBytes / sizeof(char16_t);

 protected:  // Silence Clang unused-field warning.
  char inlineStorage_[ExtensionBytes];
  HashNumber hash_;

  // Mimicking JSFatInlineString constructors.
  explicit FatInlineAtom(size_t length, JS::Latin1Char** chars,
                         js::HashNumber hash);
  explicit FatInlineAtom(size_t length, char16_t** chars, js::HashNumber hash);

 public:
  HashNumber hash() const { return hash_; }
  void initHash(HashNumber hash) { hash_ = hash; }

  inline void finalize(JS::GCContext* gcx);

  static constexpr size_t offsetOfHash() {
    static_assert(
        sizeof(FatInlineAtom) ==
            js::RoundUp(sizeof(JSThinInlineString) +
                            FatInlineAtom::ExtensionBytes + sizeof(HashNumber),
                        gc::CellAlignBytes),
        "FatInlineAtom must have size of a thin inline string + "
        "extension bytes if any + HashNumber, "
        "aligned to gc::CellAlignBytes");

    return offsetof(FatInlineAtom, hash_);
  }

  template <typename CharT>
  static bool lengthFits(size_t length) {
    return length * sizeof(CharT) <= InlineBytes;
  }
};

static_assert(sizeof(FatInlineAtom) == sizeof(JSFatInlineString),
              "FatInlineAtom must be the same size as a fat inline string");

// When an algorithm does not need a string represented as a single linear
// array of characters, this range utility may be used to traverse the string a
// sequence of linear arrays of characters. This avoids flattening ropes.
template <size_t Size = 16>
class StringSegmentRange {
  // If malloc() shows up in any profiles from this vector, we can add a new
  // StackAllocPolicy which stashes a reusable freed-at-gc buffer in the cx.
  using StackVector = JS::GCVector<JSString*, Size>;
  Rooted<StackVector> stack;
  Rooted<JSLinearString*> cur;

  bool settle(JSString* str) {
    while (str->isRope()) {
      JSRope& rope = str->asRope();
      if (!stack.append(rope.rightChild())) {
        return false;
      }
      str = rope.leftChild();
    }
    cur = &str->asLinear();
    return true;
  }

 public:
  explicit StringSegmentRange(JSContext* cx)
      : stack(cx, StackVector(cx)), cur(cx) {}

  [[nodiscard]] bool init(JSString* str) {
    MOZ_ASSERT(stack.empty());
    return settle(str);
  }

  bool empty() const { return cur == nullptr; }

  JSLinearString* front() const {
    MOZ_ASSERT(!cur->isRope());
    return cur;
  }

  [[nodiscard]] bool popFront() {
    MOZ_ASSERT(!empty());
    if (stack.empty()) {
      cur = nullptr;
      return true;
    }
    return settle(stack.popCopy());
  }
};

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

namespace js {

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
 *   - PropertyName strings which don't encode uint32_t indexes,
 *   - Symbol, and
 *   - JS::PropertyKey::isVoid.
 */
class PropertyName : public JSAtom {
 private:
  /* Vacuous and therefore unimplemented. */
  PropertyName* asPropertyName() = delete;
};

static_assert(sizeof(PropertyName) == sizeof(JSString),
              "string subclasses must be binary-compatible with JSString");

static MOZ_ALWAYS_INLINE jsid NameToId(PropertyName* name) {
  return JS::PropertyKey::NonIntAtom(name);
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
extern JSLinearString* NewString(JSContext* cx,
                                 UniquePtr<CharT[], JS::FreePolicy> chars,
                                 size_t length,
                                 js::gc::Heap heap = js::gc::Heap::Default);

/* Like NewString, but doesn't try to deflate to Latin1. */
template <js::AllowGC allowGC, typename CharT>
extern JSLinearString* NewStringDontDeflate(
    JSContext* cx, UniquePtr<CharT[], JS::FreePolicy> chars, size_t length,
    js::gc::Heap heap = js::gc::Heap::Default);

/* This may return a static string/atom or an inline string. */
extern JSLinearString* NewDependentString(
    JSContext* cx, JSString* base, size_t start, size_t length,
    js::gc::Heap heap = js::gc::Heap::Default);

/* Take ownership of an array of Latin1Chars. */
extern JSLinearString* NewLatin1StringZ(
    JSContext* cx, UniqueChars chars,
    js::gc::Heap heap = js::gc::Heap::Default);

/* Copy a counted string and GC-allocate a descriptor for it. */
template <js::AllowGC allowGC, typename CharT>
extern JSLinearString* NewStringCopyN(
    JSContext* cx, const CharT* s, size_t n,
    js::gc::Heap heap = js::gc::Heap::Default);

template <js::AllowGC allowGC>
inline JSLinearString* NewStringCopyN(
    JSContext* cx, const char* s, size_t n,
    js::gc::Heap heap = js::gc::Heap::Default) {
  return NewStringCopyN<allowGC>(cx, reinterpret_cast<const Latin1Char*>(s), n,
                                 heap);
}

template <typename CharT>
extern JSAtom* NewAtomCopyNMaybeDeflateValidLength(JSContext* cx,
                                                   const CharT* s, size_t n,
                                                   js::HashNumber hash);

template <typename CharT>
extern JSAtom* NewAtomCopyNDontDeflateValidLength(JSContext* cx, const CharT* s,
                                                  size_t n,
                                                  js::HashNumber hash);

/* Copy a counted string and GC-allocate a descriptor for it. */
template <js::AllowGC allowGC, typename CharT>
inline JSLinearString* NewStringCopy(
    JSContext* cx, mozilla::Span<const CharT> s,
    js::gc::Heap heap = js::gc::Heap::Default) {
  return NewStringCopyN<allowGC>(cx, s.data(), s.size(), heap);
}

/* Copy a counted string and GC-allocate a descriptor for it. */
template <
    js::AllowGC allowGC, typename CharT,
    typename std::enable_if_t<!std::is_same_v<CharT, unsigned char>>* = nullptr>
inline JSLinearString* NewStringCopy(
    JSContext* cx, std::basic_string_view<CharT> s,
    js::gc::Heap heap = js::gc::Heap::Default) {
  return NewStringCopyN<allowGC>(cx, s.data(), s.size(), heap);
}

/* Like NewStringCopyN, but doesn't try to deflate to Latin1. */
template <js::AllowGC allowGC, typename CharT>
extern JSLinearString* NewStringCopyNDontDeflate(
    JSContext* cx, const CharT* s, size_t n,
    js::gc::Heap heap = js::gc::Heap::Default);

template <js::AllowGC allowGC, typename CharT>
extern JSLinearString* NewStringCopyNDontDeflateNonStaticValidLength(
    JSContext* cx, const CharT* s, size_t n,
    js::gc::Heap heap = js::gc::Heap::Default);

/* Copy a C string and GC-allocate a descriptor for it. */
template <js::AllowGC allowGC>
inline JSLinearString* NewStringCopyZ(
    JSContext* cx, const char16_t* s,
    js::gc::Heap heap = js::gc::Heap::Default) {
  return NewStringCopyN<allowGC>(cx, s, js_strlen(s), heap);
}

template <js::AllowGC allowGC>
inline JSLinearString* NewStringCopyZ(
    JSContext* cx, const char* s, js::gc::Heap heap = js::gc::Heap::Default) {
  return NewStringCopyN<allowGC>(cx, s, strlen(s), heap);
}

extern JSLinearString* NewStringCopyUTF8N(
    JSContext* cx, const JS::UTF8Chars& utf8, JS::SmallestEncoding encoding,
    js::gc::Heap heap = js::gc::Heap::Default);

extern JSLinearString* NewStringCopyUTF8N(
    JSContext* cx, const JS::UTF8Chars& utf8,
    js::gc::Heap heap = js::gc::Heap::Default);

inline JSLinearString* NewStringCopyUTF8Z(
    JSContext* cx, const JS::ConstUTF8CharsZ utf8,
    js::gc::Heap heap = js::gc::Heap::Default) {
  return NewStringCopyUTF8N(
      cx, JS::UTF8Chars(utf8.c_str(), strlen(utf8.c_str())), heap);
}

template <typename CharT>
JSString* NewMaybeExternalString(JSContext* cx, const CharT* s, size_t n,
                                 const JSExternalStringCallbacks* callbacks,
                                 bool* allocatedExternal,
                                 js::gc::Heap heap = js::gc::Heap::Default);

static_assert(sizeof(HashNumber) == 4);

template <AllowGC allowGC>
extern JSString* ConcatStrings(
    JSContext* cx, typename MaybeRooted<JSString*, allowGC>::HandleType left,
    typename MaybeRooted<JSString*, allowGC>::HandleType right,
    js::gc::Heap heap = js::gc::Heap::Default);

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
extern bool EqualStrings(const JSLinearString* str1,
                         const JSLinearString* str2);

/**
 * Compare two strings that are known to be the same length.
 * Exposed for the JITs; for ordinary uses, EqualStrings() is more sensible.
 *
 * The caller must have checked for the following cases that can be handled
 * efficiently without requiring a character comparison:
 *   - str1 == str2
 *   - str1->length() != str2->length()
 *   - str1->isAtom() && str2->isAtom()
 */
extern bool EqualChars(const JSLinearString* str1, const JSLinearString* str2);

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
 * Compare two strings, like CompareChars.
 */
extern int32_t CompareStrings(const JSLinearString* str1,
                              const JSLinearString* str2);

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
   *
   * Note: keep this in sync with MacroAssembler::loadStringChar and
   * CanAttachStringChar.
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

MOZ_ALWAYS_INLINE bool JSString::getCodePoint(JSContext* cx, size_t index,
                                              char32_t* code) {
  // C++ implementation of https://tc39.es/ecma262/#sec-codepointat
  size_t size = length();
  MOZ_ASSERT(index < size);

  char16_t first;
  if (!getChar(cx, index, &first)) {
    return false;
  }
  if (!js::unicode::IsLeadSurrogate(first) || index + 1 == size) {
    *code = first;
    return true;
  }

  char16_t second;
  if (!getChar(cx, index + 1, &second)) {
    return false;
  }
  if (!js::unicode::IsTrailSurrogate(second)) {
    *code = first;
    return true;
  }

  *code = js::unicode::UTF16Decode(first, second);
  return true;
}

MOZ_ALWAYS_INLINE JSLinearString* JSString::ensureLinear(JSContext* cx) {
  return isLinear() ? &asLinear() : asRope().flatten(cx);
}

inline JSLinearString* JSString::base() const {
  MOZ_ASSERT(hasBase());
  MOZ_ASSERT_IF(!isAtomRef(), !d.s.u3.base->isInline());
  MOZ_ASSERT(d.s.u3.base->assertIsValidBase());
  if (isAtomRef()) {
    return static_cast<JSLinearString*>(d.s.u3.atom);
  }
  return d.s.u3.base;
}

inline JSAtom* JSString::atom() const {
  MOZ_ASSERT(isAtomRef());
  return d.s.u3.atom;
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
MOZ_ALWAYS_INLINE bool js::ThinInlineAtom::lengthFits<JS::Latin1Char>(
    size_t length) {
  return length <= MAX_LENGTH_LATIN1;
}

template <>
MOZ_ALWAYS_INLINE bool js::ThinInlineAtom::lengthFits<char16_t>(size_t length) {
  return length <= MAX_LENGTH_TWO_BYTE;
}

template <>
MOZ_ALWAYS_INLINE bool js::FatInlineAtom::lengthFits<JS::Latin1Char>(
    size_t length) {
  return length <= MAX_LENGTH_LATIN1;
}

template <>
MOZ_ALWAYS_INLINE bool js::FatInlineAtom::lengthFits<char16_t>(size_t length) {
  return length <= MAX_LENGTH_TWO_BYTE;
}

template <>
MOZ_ALWAYS_INLINE bool JSAtom::lengthFitsInline<JS::Latin1Char>(size_t length) {
  // If it fits in a fat inline atom, it fits in any inline atom.
  return js::FatInlineAtom::lengthFits<JS::Latin1Char>(length);
}

template <>
MOZ_ALWAYS_INLINE bool JSAtom::lengthFitsInline<char16_t>(size_t length) {
  // If it fits in a fat inline atom, it fits in any inline atom.
  return js::FatInlineAtom::lengthFits<char16_t>(length);
}

template <>
MOZ_ALWAYS_INLINE void JSString::setNonInlineChars(const char16_t* chars) {
  // Check that the new buffer is located in the StringBufferArena
  if (!(isAtomRef() && atom()->isInline())) {
    checkStringCharsArena(chars);
  }
  d.s.u2.nonInlineCharsTwoByte = chars;
}

template <>
MOZ_ALWAYS_INLINE void JSString::setNonInlineChars(
    const JS::Latin1Char* chars) {
  // Check that the new buffer is located in the StringBufferArena
  if (!(isAtomRef() && atom()->isInline())) {
    checkStringCharsArena(chars);
  }
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

  void setNext(StringRelocationOverlay* next) {
    MOZ_ASSERT(isForwarded());
    next_ = next;
  }

  StringRelocationOverlay* next() const {
    MOZ_ASSERT(isForwarded());
    return (StringRelocationOverlay*)next_;
  }

  template <typename CharT>
  MOZ_ALWAYS_INLINE const CharT* savedNurseryChars() const;

  const MOZ_ALWAYS_INLINE JS::Latin1Char* savedNurseryCharsLatin1() const {
    MOZ_ASSERT(!forwardingAddress()->as<JSString>()->hasBase());
    return nurseryCharsLatin1;
  }

  const MOZ_ALWAYS_INLINE char16_t* savedNurseryCharsTwoByte() const {
    MOZ_ASSERT(!forwardingAddress()->as<JSString>()->hasBase());
    return nurseryCharsTwoByte;
  }

  JSLinearString* savedNurseryBaseOrRelocOverlay() const {
    MOZ_ASSERT(forwardingAddress()->as<JSString>()->hasBase());
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
