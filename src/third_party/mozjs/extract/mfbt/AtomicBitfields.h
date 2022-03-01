/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_AtomicBitfields_h
#define mozilla_AtomicBitfields_h

#include "mozilla/Assertions.h"
#include "mozilla/MacroArgs.h"
#include "mozilla/MacroForEach.h"

#include <limits>
#include <stdint.h>
#include <type_traits>

#ifdef __wasi__
#  include "mozilla/WasiAtomic.h"
#else
#  include <atomic>
#endif  // __wasi__

namespace mozilla {

// Creates a series of atomic bitfields.
//
// |aBitfields| is the name of the underlying storage for the bitfields.
// |aBitFieldsSize| is the size of the underlying storage (8, 16, 32, or 64).
//
// Bitfields are specified as a triplet of (type, name, size), which mirrors
// the way you declare native C++ bitfields (bool mMyField1: 1). Trailing
// commas are not supported in the list of bitfields.
//
// Signed integer types are not supported by this Macro to avoid dealing with
// packing/unpacking the sign bit and C++'s general messiness around signed
// integer representations not being fully defined.
//
// You cannot request a single field that's the
// size of the the entire bitfield storage. Just use a normal atomic integer!
//
//
// ========================== SEMANTICS AND SAFETY ============================
//
// All fields are default-initialized to 0.
//
// In debug builds, storing a value to a bitfield that's larger than its bits
// can fit will trigger an assertion. In release builds, the value will just be
// masked off.
//
// If you request anything unsupported by this macro it should result in
// a compile-time error (either a static assert or just weird macro errors).
// For instance, this macro will statically prevent using more bits than
// |aBitFieldsSize|, so specifying the size is just to prevent accidentally
// making the storage bigger.
//
// Each field will get a Load$NAME and Store$Name method which will atomically
// load and store the requested value with a Sequentially Consistent memory
// order (to be on the safe side). Storing a field requires a compare-exchange,
// so a thread may get stalled if there's a lot of contention on the bitfields.
//
//
// ============================== MOTIVATION ==================================
//
// You might be wondering: why would I need atomic bitfields? Well as it turns
// out, bitfields and concurrency mess a lot of people up!
//
// CPUs don't have operations to write to a handful of bits -- they generally
// only have the precision of a byte. So when you use C++'s native bitfields,
// the compiler generates code to mask and shift the values in for you. This
// means writing to a single field will actually overwrite all the other
// bitfields that are packed in with it!
//
// In single-threaded code this is fine; the old values are loaded and written
// back by the compiler's generated code. But in concurrent code, it means
// that accessing two different fields can be an unexpected Data Race (which is
// Undefined Behavior!).
//
// By using MOZ_ATOMIC_BITFIELDS, you protect yourself from these Data Races,
// and don't have to worry about writes getting lost.
//
//
// ================================ EXAMPLE ===================================
//
//   #include "mozilla/AtomicBitfields.h"
//   #include <stdint.h>
//
//
//   struct MyType {
//     MOZ_ATOMIC_BITFIELDS(mAtomicFields, 8, (
//      (bool, IsDownloaded, 1),
//      (uint32_t, SomeData, 2),
//      (uint8_t, OtherData, 5)
//     ))
//
//     int32_t aNormalInteger;
//
//     explicit MyType(uint32_t aSomeData): aNormalInteger(7) {
//       StoreSomeData(aSomeData);
//       // Other bitfields were already default initialized to 0/false
//     }
//   };
//
//
//   int main() {
//     MyType val(3);
//
//     if (!val.LoadIsDownloaded()) {
//       val.StoreOtherData(2);
//       val.StoreIsDownloaded(true);
//     }
//   }
//
//
// ============================== GENERATED ===================================
//
// This macro is a real mess to read because, well, it's a macro. So for the
// sake of anyone who has to review or modify its internals, here's a rough
// sketch of what the above example would expand to:
//
//   struct MyType {
//     // The actual storage of the bitfields, initialized to 0.
//     std::atomic_uint8_t mAtomicFields{0};
//
//     // How many bits were actually used (in this case, all of them).
//     static const size_t mAtomicFields_USED_BITS = 8;
//
//     // The offset values for each field.
//     static const size_t mAtomicFieldsIsDownloaded = 0;
//     static const size_t mAtomicFieldsSomeData = 1;
//     static const size_t mAtomicFieldsOtherData = 3;
//
//     // Quick safety guard to prevent capacity overflow.
//     static_assert(mAtomicFields_USED_BITS <= 8);
//
//     // Asserts that fields are reasonable.
//     static_assert(8>1, "mAtomicFields: MOZ_ATOMIC_BITFIELDS field too big");
//     static_assert(std::is_unsigned<bool>(), "mAtomicFields:
//     MOZ_ATOMIC_BITFIELDS doesn't support signed payloads");
//     // ...and so on
//
//     // Load/Store methods for all the fields.
//
//     bool LoadIsDownloaded() { ... }
//     void StoreIsDownloaded(bool aValue) { ... }
//
//     uint32_t LoadSomeData() { ... }
//     void StoreSomeData(uint32_t aValue) { ... }
//
//     uint8_t LoadOtherData() { ... }
//     void StoreOtherData(uint8_t aValue) { ... }
//
//
//     // Remainder of the struct body continues normally.
//     int32_t aNormalInteger;
//     explicit MyType(uint32_t aSomeData): aNormalInteger(7) {
//       StoreSomeData(aSomeData);
//       // Other bitfields were already default initialized to 0/false.
//     }
//   }
//
// Also if you're wondering why there's so many MOZ_CONCAT's -- it's because
// the preprocessor sometimes gets confused if we use ## on certain arguments.
// MOZ_CONCAT reliably kept the preprocessor happy, sorry it's so ugly!
//
//
// ==================== FIXMES / FUTURE WORK ==================================
//
// * It would be nice if LoadField could be IsField for booleans.
//
// * For the case of setting something to all 1's or 0's, we can use
//   |fetch_or| or |fetch_and| instead of |compare_exchange_weak|. Is this
//   worth providing? (Possibly for 1-bit boolean fields?)
//
// * Try harder to hide the atomic/enum/array internals from
//   the outer struct?
//
#define MOZ_ATOMIC_BITFIELDS(aBitfields, aBitfieldsSize, aFields)             \
  std::atomic_uint##aBitfieldsSize##_t aBitfields{0};                         \
                                                                              \
  static const size_t MOZ_CONCAT(aBitfields, _USED_BITS) =                    \
      MOZ_FOR_EACH_SEPARATED(MOZ_ATOMIC_BITFIELDS_FIELD_SIZE, (+), (),        \
                             aFields);                                        \
                                                                              \
  MOZ_ROLL_EACH(MOZ_ATOMIC_BITFIELDS_OFFSET_HELPER1, (aBitfields, ), aFields) \
                                                                              \
  static_assert(MOZ_CONCAT(aBitfields, _USED_BITS) <= aBitfieldsSize,         \
                #aBitfields ": Maximum bits (" #aBitfieldsSize                \
                            ") exceeded for MOZ_ATOMIC_BITFIELDS instance");  \
                                                                              \
  MOZ_FOR_EACH(MOZ_ATOMIC_BITFIELDS_FIELD_HELPER,                             \
               (aBitfields, aBitfieldsSize, ), aFields)

// Just a helper to unpack the head of the list.
#define MOZ_ATOMIC_BITFIELDS_OFFSET_HELPER1(aBitfields, aFields) \
  MOZ_ATOMIC_BITFIELDS_OFFSET_HELPER2(aBitfields, MOZ_ARG_1 aFields, aFields);

// Just a helper to unpack the name and call the real function.
#define MOZ_ATOMIC_BITFIELDS_OFFSET_HELPER2(aBitfields, aField, aFields) \
  MOZ_ATOMIC_BITFIELDS_OFFSET(aBitfields, MOZ_ARG_2 aField, aFields)

// To compute the offset of a field, why sum up all the offsets after it
// (inclusive) and subtract that from the total sum itself. We do this to swap
// the rolling sum that |MOZ_ROLL_EACH| gets us from descending to ascending.
#define MOZ_ATOMIC_BITFIELDS_OFFSET(aBitfields, aFieldName, aFields)    \
  static const size_t MOZ_CONCAT(aBitfields, aFieldName) =              \
      MOZ_CONCAT(aBitfields, _USED_BITS) -                              \
      (MOZ_FOR_EACH_SEPARATED(MOZ_ATOMIC_BITFIELDS_FIELD_SIZE, (+), (), \
                              aFields));

// Just a more clearly named way of unpacking the size.
#define MOZ_ATOMIC_BITFIELDS_FIELD_SIZE(aArgs) MOZ_ARG_3 aArgs

// Just a helper to unpack the tuple and call the real function.
#define MOZ_ATOMIC_BITFIELDS_FIELD_HELPER(aBitfields, aBitfieldsSize, aArgs) \
  MOZ_ATOMIC_BITFIELDS_FIELD(aBitfields, aBitfieldsSize, MOZ_ARG_1 aArgs,    \
                             MOZ_ARG_2 aArgs, MOZ_ARG_3 aArgs)

// We need to disable this with coverity because it doesn't like checking that
// booleans are < 2 (because they always are).
#ifdef __COVERITY__
#  define MOZ_ATOMIC_BITFIELDS_STORE_GUARD(aValue, aFieldSize)
#else
#  define MOZ_ATOMIC_BITFIELDS_STORE_GUARD(aValue, aFieldSize) \
    MOZ_ASSERT(((uint64_t)aValue) < (1ull << aFieldSize),      \
               "Stored value exceeded capacity of bitfield!")
#endif

// Generates the Load and Store methods for each field.
//
// Some comments here because inline macro comments are a pain in the neck:
//
// Most of the locals are forward declared to minimize messy macroified
// type declaration. Also a lot of locals are used to try to make things
// a little more clear, while also avoiding integer promotion issues.
// This is why some locals are literally just copying a value we already have:
// to force it to the right size.
//
// There's an annoying overflow case where a bitfields instance has a field
// that is the same size as the bitfields. Rather than trying to handle that,
// we just static_assert against it.
//
//
// BITMATH EXPLAINED:
//
// For |Load$Name|:
//
//    mask = ((1 << fieldSize) - 1) << offset
//
// If you subtract 1 from a value with 1 bit set you get all 1's below that bit.
// This is perfect for ANDing out |fieldSize| bits. We shift by |offset| to get
// it in the right place.
//
//    value = (aBitfields.load() & mask) >> offset
//
// This sets every bit we're not interested in to 0. Shifting the result by
// |offset| converts the value back to its native format, ready to be cast
// up to an integer type.
//
//
// For |Store$Name|:
//
//    packedValue = (resizedValue << offset) & mask
//
// This converts a native value to the packed format. If the value is in bounds,
// the AND will do nothing. If it's out of bounds (not checked in release),
// then it will cause the value to wrap around by modulo 2^aFieldSize, just like
// a normal uint.
//
//    clearedValue = oldValue & ~mask;
//
// This clears the bits where our field is stored on our bitfield storage by
// ANDing it with an inverted (NOTed) mask.
//
//    newValue = clearedValue | packedValue;
//
// Once we have |packedValue| and |clearedValue| they just need to be ORed
// together to merge the new field value with the old values of all the other
// fields.
//
// This last step is done in a while loop because someone else can modify
// the bits before we have a chance to. If we didn't guard against this,
// our write would undo the write the other thread did. |compare_exchange_weak|
// is specifically designed to handle this. We give it what we expect the
// current value to be, and what we want it to be. If someone else modifies
// the bitfields before us, then we will reload the value and try again.
//
// Note that |compare_exchange_weak| writes back the actual value to the
// "expected" argument (it's passed by-reference), so we don't need to do
// another load in the body of the loop when we fail to write our result.
#define MOZ_ATOMIC_BITFIELDS_FIELD(aBitfields, aBitfieldsSize, aFieldType, \
                                   aFieldName, aFieldSize)                 \
  static_assert(aBitfieldsSize > aFieldSize,                               \
                #aBitfields ": MOZ_ATOMIC_BITFIELDS field too big");       \
  static_assert(std::is_unsigned<aFieldType>(), #aBitfields                \
                ": MOZ_ATOMIC_BITFIELDS doesn't support signed payloads"); \
                                                                           \
  aFieldType MOZ_CONCAT(Load, aFieldName)() const {                        \
    uint##aBitfieldsSize##_t fieldSize, mask, masked, value;               \
    size_t offset = MOZ_CONCAT(aBitfields, aFieldName);                    \
    fieldSize = aFieldSize;                                                \
    mask = ((1ull << fieldSize) - 1ull) << offset;                         \
    masked = aBitfields.load() & mask;                                     \
    value = (masked >> offset);                                            \
    return value;                                                          \
  }                                                                        \
                                                                           \
  void MOZ_CONCAT(Store, aFieldName)(aFieldType aValue) {                  \
    MOZ_ATOMIC_BITFIELDS_STORE_GUARD(aValue, aFieldSize);                  \
    uint##aBitfieldsSize##_t fieldSize, mask, resizedValue, packedValue,   \
        oldValue, clearedValue, newValue;                                  \
    size_t offset = MOZ_CONCAT(aBitfields, aFieldName);                    \
    fieldSize = aFieldSize;                                                \
    mask = ((1ull << fieldSize) - 1ull) << offset;                         \
    resizedValue = aValue;                                                 \
    packedValue = (resizedValue << offset) & mask;                         \
    oldValue = aBitfields.load();                                          \
    do {                                                                   \
      clearedValue = oldValue & ~mask;                                     \
      newValue = clearedValue | packedValue;                               \
    } while (!aBitfields.compare_exchange_weak(oldValue, newValue));       \
  }

// OK SO THIS IS A GROSS HACK. GCC 10.2 (and below) has a bug[1] where it
// doesn't allow a static array to reference itself in its initializer, so we
// need to create a hacky way to produce a rolling sum of all the offsets.
//
// To do this, we make a tweaked version of |MOZ_FOR_EACH| which instead of
// passing just one argument to |aMacro| it passes the remaining values of
// |aArgs|.
//
// This allows us to expand an input (a, b, c, d) quadratically to:
//
// int sum1 = a + b + c + d;
// int sum2 = b + c + d;
// int sum3 = c + d;
// int sum4 = d;
//
// So all of this is a copy-paste of |MOZ_FOR_EACH| except the definition
// of |MOZ_FOR_EACH_HELPER| no longer extracts an argument with |MOZ_ARG_1|.
// Also this is restricted to 32 arguments just to reduce footprint a little.
//
// If the GCC bug is ever fixed, then this hack can be removed, and we can
// use the non-quadratic version that was originally written[2]. In case
// that link dies, a brief summary of that implementation:
//
// * Associate each field with an index by creating an `enum class` with
//   entries for each field (an existing gecko patten).
//
// * Calculate offsets with a constexpr static array whose initializer
//   self-referentially adds the contents of the previous index to the
//   compute the current one.
//
// * Index into this array with the enum.
//
// [1] https://gcc.gnu.org/bugzilla/show_bug.cgi?id=97234
// [2]: https://phabricator.services.mozilla.com/D91622?id=346499
#define MOZ_ROLL_EACH_EXPAND_HELPER(...) __VA_ARGS__
#define MOZ_ROLL_EACH_GLUE(a, b) a b
#define MOZ_ROLL_EACH_SEPARATED(aMacro, aSeparator, aFixedArgs, aArgs)       \
  MOZ_ROLL_EACH_GLUE(MOZ_PASTE_PREFIX_AND_ARG_COUNT(                         \
                         MOZ_ROLL_EACH_, MOZ_ROLL_EACH_EXPAND_HELPER aArgs), \
                     (aMacro, aSeparator, aFixedArgs, aArgs))
#define MOZ_ROLL_EACH(aMacro, aFixedArgs, aArgs) \
  MOZ_ROLL_EACH_SEPARATED(aMacro, (), aFixedArgs, aArgs)

#define MOZ_ROLL_EACH_HELPER_GLUE(a, b) a b
#define MOZ_ROLL_EACH_HELPER(aMacro, aFixedArgs, aArgs) \
  MOZ_ROLL_EACH_HELPER_GLUE(aMacro,                     \
                            (MOZ_ROLL_EACH_EXPAND_HELPER aFixedArgs aArgs))

#define MOZ_ROLL_EACH_0(m, s, fa, a)
#define MOZ_ROLL_EACH_1(m, s, fa, a) MOZ_ROLL_EACH_HELPER(m, fa, a)
#define MOZ_ROLL_EACH_2(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)     \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_1(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_3(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)     \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_2(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_4(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)     \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_3(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_5(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)     \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_4(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_6(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)     \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_5(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_7(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)     \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_6(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_8(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)     \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_7(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_9(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)     \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_8(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_10(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_9(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_11(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_10(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_12(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_11(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_13(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_12(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_14(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_13(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_15(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_14(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_16(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_15(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_17(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_16(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_18(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_17(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_19(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_18(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_20(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_19(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_21(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_20(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_22(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_21(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_23(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_22(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_24(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_23(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_25(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_24(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_26(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_25(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_27(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_26(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_28(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_27(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_29(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_28(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_30(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_29(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_31(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_30(m, s, fa, (MOZ_ARGS_AFTER_1 a))
#define MOZ_ROLL_EACH_32(m, s, fa, a) \
  MOZ_ROLL_EACH_HELPER(m, fa, a)      \
  MOZ_ROLL_EACH_EXPAND_HELPER s MOZ_ROLL_EACH_31(m, s, fa, (MOZ_ARGS_AFTER_1 a))
}  // namespace mozilla
#endif /* mozilla_AtomicBitfields_h */
