/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Functions for reading and writing integers in various endiannesses. */

/*
 * The classes LittleEndian and BigEndian expose static methods for
 * reading and writing 16-, 32-, and 64-bit signed and unsigned integers
 * in their respective endianness.  The naming scheme is:
 *
 * {Little,Big}Endian::{read,write}{Uint,Int}<bitsize>
 *
 * For instance, LittleEndian::readInt32 will read a 32-bit signed
 * integer from memory in little endian format.  Similarly,
 * BigEndian::writeUint16 will write a 16-bit unsigned integer to memory
 * in big-endian format.
 *
 * The class NativeEndian exposes methods for conversion of existing
 * data to and from the native endianness.  These methods are intended
 * for cases where data needs to be transferred, serialized, etc.
 * swap{To,From}{Little,Big}Endian byteswap a single value if necessary.
 * Bulk conversion functions are also provided which optimize the
 * no-conversion-needed case:
 *
 * - copyAndSwap{To,From}{Little,Big}Endian;
 * - swap{To,From}{Little,Big}EndianInPlace.
 *
 * The *From* variants are intended to be used for reading data and the
 * *To* variants for writing data.
 *
 * Methods on NativeEndian work with integer data of any type.
 * Floating-point data is not supported.
 *
 * For clarity in networking code, "Network" may be used as a synonym
 * for "Big" in any of the above methods or class names.
 *
 * As an example, reading a file format header whose fields are stored
 * in big-endian format might look like:
 *
 * class ExampleHeader
 * {
 * private:
 *   uint32_t mMagic;
 *   uint32_t mLength;
 *   uint32_t mTotalRecords;
 *   uint64_t mChecksum;
 *
 * public:
 *   ExampleHeader(const void* data)
 *   {
 *     const uint8_t* ptr = static_cast<const uint8_t*>(data);
 *     mMagic = BigEndian::readUint32(ptr); ptr += sizeof(uint32_t);
 *     mLength = BigEndian::readUint32(ptr); ptr += sizeof(uint32_t);
 *     mTotalRecords = BigEndian::readUint32(ptr); ptr += sizeof(uint32_t);
 *     mChecksum = BigEndian::readUint64(ptr);
 *   }
 *   ...
 * };
 */

#ifndef mozilla_Endian_h
#define mozilla_Endian_h

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/Compiler.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/TypeTraits.h"

#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER)
#  include <stdlib.h>
#  pragma intrinsic(_byteswap_ushort)
#  pragma intrinsic(_byteswap_ulong)
#  pragma intrinsic(_byteswap_uint64)
#endif

#if defined(_WIN64)
#  if defined(_M_X64) || defined(_M_AMD64) || defined(_AMD64_)
#    define MOZ_LITTLE_ENDIAN 1
#  else
#    error "CPU type is unknown"
#  endif
#elif defined(_WIN32)
#  if defined(_M_IX86)
#    define MOZ_LITTLE_ENDIAN 1
#  elif defined(_M_ARM)
#    define MOZ_LITTLE_ENDIAN 1
#  else
#    error "CPU type is unknown"
#  endif
#elif defined(__APPLE__) || defined(__powerpc__) || defined(__ppc__)
#  if __LITTLE_ENDIAN__
#    define MOZ_LITTLE_ENDIAN 1
#  elif __BIG_ENDIAN__
#    define MOZ_BIG_ENDIAN 1
#  endif
#elif defined(__GNUC__) && \
      defined(__BYTE_ORDER__) && \
      defined(__ORDER_LITTLE_ENDIAN__) && \
      defined(__ORDER_BIG_ENDIAN__)
   /*
    * Some versions of GCC provide architecture-independent macros for
    * this.  Yes, there are more than two values for __BYTE_ORDER__.
    */
#  if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#    define MOZ_LITTLE_ENDIAN 1
#  elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#    define MOZ_BIG_ENDIAN 1
#  else
#    error "Can't handle mixed-endian architectures"
#  endif
/*
 * We can't include useful headers like <endian.h> or <sys/isa_defs.h>
 * here because they're not present on all platforms.  Instead we have
 * this big conditional that ideally will catch all the interesting
 * cases.
 */
#elif defined(__sparc) || defined(__sparc__) || \
      defined(_POWER) || defined(__hppa) || \
      defined(_MIPSEB) || defined(__ARMEB__) || \
      defined(__s390__) || defined(__AARCH64EB__) || \
      (defined(__sh__) && defined(__LITTLE_ENDIAN__)) || \
      (defined(__ia64) && defined(__BIG_ENDIAN__))
#  define MOZ_BIG_ENDIAN 1
#elif defined(__i386) || defined(__i386__) || \
      defined(__x86_64) || defined(__x86_64__) || \
      defined(_MIPSEL) || defined(__ARMEL__) || \
      defined(__alpha__) || defined(__AARCH64EL__) || \
      (defined(__sh__) && defined(__BIG_ENDIAN__)) || \
      (defined(__ia64) && !defined(__BIG_ENDIAN__))
#  define MOZ_LITTLE_ENDIAN 1
#endif

#if MOZ_BIG_ENDIAN
#  define MOZ_LITTLE_ENDIAN 0
#elif MOZ_LITTLE_ENDIAN
#  define MOZ_BIG_ENDIAN 0
#else
#  error "Cannot determine endianness"
#endif

#if defined(__clang__)
#  if __has_builtin(__builtin_bswap16)
#    define MOZ_HAVE_BUILTIN_BYTESWAP16 __builtin_bswap16
#  endif
#elif defined(__GNUC__)
#  if MOZ_GCC_VERSION_AT_LEAST(4, 8, 0)
#    define MOZ_HAVE_BUILTIN_BYTESWAP16 __builtin_bswap16
#  endif
#elif defined(_MSC_VER)
#    define MOZ_HAVE_BUILTIN_BYTESWAP16 _byteswap_ushort
#endif

namespace mozilla {

namespace detail {

/*
 * We need wrappers here because free functions with default template
 * arguments and/or partial specialization of function templates are not
 * supported by all the compilers we use.
 */
template<typename T, size_t Size = sizeof(T)>
struct Swapper;

template<typename T>
struct Swapper<T, 2>
{
  static T swap(T aValue)
  {
#if defined(MOZ_HAVE_BUILTIN_BYTESWAP16)
    return MOZ_HAVE_BUILTIN_BYTESWAP16(aValue);
#else
    return T(((aValue & 0x00ff) << 8) | ((aValue & 0xff00) >> 8));
#endif
  }
};

template<typename T>
struct Swapper<T, 4>
{
  static T swap(T aValue)
  {
#if defined(__clang__) || defined(__GNUC__)
    return T(__builtin_bswap32(aValue));
#elif defined(_MSC_VER)
    return T(_byteswap_ulong(aValue));
#else
    return T(((aValue & 0x000000ffU) << 24) |
             ((aValue & 0x0000ff00U) << 8) |
             ((aValue & 0x00ff0000U) >> 8) |
             ((aValue & 0xff000000U) >> 24));
#endif
  }
};

template<typename T>
struct Swapper<T, 8>
{
  static inline T swap(T aValue)
  {
#if defined(__clang__) || defined(__GNUC__)
    return T(__builtin_bswap64(aValue));
#elif defined(_MSC_VER)
    return T(_byteswap_uint64(aValue));
#else
    return T(((aValue & 0x00000000000000ffULL) << 56) |
             ((aValue & 0x000000000000ff00ULL) << 40) |
             ((aValue & 0x0000000000ff0000ULL) << 24) |
             ((aValue & 0x00000000ff000000ULL) << 8) |
             ((aValue & 0x000000ff00000000ULL) >> 8) |
             ((aValue & 0x0000ff0000000000ULL) >> 24) |
             ((aValue & 0x00ff000000000000ULL) >> 40) |
             ((aValue & 0xff00000000000000ULL) >> 56));
#endif
  }
};

enum Endianness { Little, Big };

#if MOZ_BIG_ENDIAN
#  define MOZ_NATIVE_ENDIANNESS detail::Big
#else
#  define MOZ_NATIVE_ENDIANNESS detail::Little
#endif

class EndianUtils
{
  /**
   * Assert that the memory regions [aDest, aDest+aCount) and
   * [aSrc, aSrc+aCount] do not overlap.  aCount is given in bytes.
   */
  static void assertNoOverlap(const void* aDest, const void* aSrc,
                              size_t aCount)
  {
    DebugOnly<const uint8_t*> byteDestPtr = static_cast<const uint8_t*>(aDest);
    DebugOnly<const uint8_t*> byteSrcPtr = static_cast<const uint8_t*>(aSrc);
    MOZ_ASSERT((byteDestPtr <= byteSrcPtr &&
                byteDestPtr + aCount <= byteSrcPtr) ||
               (byteSrcPtr <= byteDestPtr &&
                byteSrcPtr + aCount <= byteDestPtr));
  }

  template<typename T>
  static void assertAligned(T* aPtr)
  {
    MOZ_ASSERT((uintptr_t(aPtr) % sizeof(T)) == 0, "Unaligned pointer!");
  }

protected:
  /**
   * Return |aValue| converted from SourceEndian encoding to DestEndian
   * encoding.
   */
  template<Endianness SourceEndian, Endianness DestEndian, typename T>
  static inline T maybeSwap(T aValue)
  {
    if (SourceEndian == DestEndian) {
      return aValue;
    }
    return Swapper<T>::swap(aValue);
  }

  /**
   * Convert |aCount| elements at |aPtr| from SourceEndian encoding to
   * DestEndian encoding.
   */
  template<Endianness SourceEndian, Endianness DestEndian, typename T>
  static inline void maybeSwapInPlace(T* aPtr, size_t aCount)
  {
    assertAligned(aPtr);

    if (SourceEndian == DestEndian) {
      return;
    }
    for (size_t i = 0; i < aCount; i++) {
      aPtr[i] = Swapper<T>::swap(aPtr[i]);
    }
  }

  /**
   * Write |aCount| elements to the unaligned address |aDest| in DestEndian
   * format, using elements found at |aSrc| in SourceEndian format.
   */
  template<Endianness SourceEndian, Endianness DestEndian, typename T>
  static void copyAndSwapTo(void* aDest, const T* aSrc, size_t aCount)
  {
    assertNoOverlap(aDest, aSrc, aCount * sizeof(T));
    assertAligned(aSrc);

    if (SourceEndian == DestEndian) {
      memcpy(aDest, aSrc, aCount * sizeof(T));
      return;
    }

    uint8_t* byteDestPtr = static_cast<uint8_t*>(aDest);
    for (size_t i = 0; i < aCount; ++i) {
      union
      {
        T mVal;
        uint8_t mBuffer[sizeof(T)];
      } u;
      u.mVal = maybeSwap<SourceEndian, DestEndian>(aSrc[i]);
      memcpy(byteDestPtr, u.mBuffer, sizeof(T));
      byteDestPtr += sizeof(T);
    }
  }

  /**
   * Write |aCount| elements to |aDest| in DestEndian format, using elements
   * found at the unaligned address |aSrc| in SourceEndian format.
   */
  template<Endianness SourceEndian, Endianness DestEndian, typename T>
  static void copyAndSwapFrom(T* aDest, const void* aSrc, size_t aCount)
  {
    assertNoOverlap(aDest, aSrc, aCount * sizeof(T));
    assertAligned(aDest);

    if (SourceEndian == DestEndian) {
      memcpy(aDest, aSrc, aCount * sizeof(T));
      return;
    }

    const uint8_t* byteSrcPtr = static_cast<const uint8_t*>(aSrc);
    for (size_t i = 0; i < aCount; ++i) {
      union
      {
        T mVal;
        uint8_t mBuffer[sizeof(T)];
      } u;
      memcpy(u.mBuffer, byteSrcPtr, sizeof(T));
      aDest[i] = maybeSwap<SourceEndian, DestEndian>(u.mVal);
      byteSrcPtr += sizeof(T);
    }
  }
};

template<Endianness ThisEndian>
class Endian : private EndianUtils
{
protected:
  /** Read a uint16_t in ThisEndian endianness from |aPtr| and return it. */
  static MOZ_WARN_UNUSED_RESULT uint16_t readUint16(const void* aPtr)
  {
    return read<uint16_t>(aPtr);
  }

  /** Read a uint32_t in ThisEndian endianness from |aPtr| and return it. */
  static MOZ_WARN_UNUSED_RESULT uint32_t readUint32(const void* aPtr)
  {
    return read<uint32_t>(aPtr);
  }

  /** Read a uint64_t in ThisEndian endianness from |aPtr| and return it. */
  static MOZ_WARN_UNUSED_RESULT uint64_t readUint64(const void* aPtr)
  {
    return read<uint64_t>(aPtr);
  }

  /** Read an int16_t in ThisEndian endianness from |aPtr| and return it. */
  static MOZ_WARN_UNUSED_RESULT int16_t readInt16(const void* aPtr)
  {
    return read<int16_t>(aPtr);
  }

  /** Read an int32_t in ThisEndian endianness from |aPtr| and return it. */
  static MOZ_WARN_UNUSED_RESULT int32_t readInt32(const void* aPtr)
  {
    return read<uint32_t>(aPtr);
  }

  /** Read an int64_t in ThisEndian endianness from |aPtr| and return it. */
  static MOZ_WARN_UNUSED_RESULT int64_t readInt64(const void* aPtr)
  {
    return read<int64_t>(aPtr);
  }

  /** Write |aValue| to |aPtr| using ThisEndian endianness. */
  static void writeUint16(void* aPtr, uint16_t aValue)
  {
    write(aPtr, aValue);
  }

  /** Write |aValue| to |aPtr| using ThisEndian endianness. */
  static void writeUint32(void* aPtr, uint32_t aValue)
  {
    write(aPtr, aValue);
  }

  /** Write |aValue| to |aPtr| using ThisEndian endianness. */
  static void writeUint64(void* aPtr, uint64_t aValue)
  {
    write(aPtr, aValue);
  }

  /** Write |aValue| to |aPtr| using ThisEndian endianness. */
  static void writeInt16(void* aPtr, int16_t aValue)
  {
    write(aPtr, aValue);
  }

  /** Write |aValue| to |aPtr| using ThisEndian endianness. */
  static void writeInt32(void* aPtr, int32_t aValue)
  {
    write(aPtr, aValue);
  }

  /** Write |aValue| to |aPtr| using ThisEndian endianness. */
  static void writeInt64(void* aPtr, int64_t aValue)
  {
    write(aPtr, aValue);
  }

  /*
   * Converts a value of type T to little-endian format.
   *
   * This function is intended for cases where you have data in your
   * native-endian format and you need it to appear in little-endian
   * format for transmission.
   */
  template<typename T>
  MOZ_WARN_UNUSED_RESULT static T swapToLittleEndian(T aValue)
  {
    return maybeSwap<ThisEndian, Little>(aValue);
  }

  /*
   * Copies |aCount| values of type T starting at |aSrc| to |aDest|, converting
   * them to little-endian format if ThisEndian is Big.
   * As with memcpy, |aDest| and |aSrc| must not overlap.
   */
  template<typename T>
  static void copyAndSwapToLittleEndian(void* aDest, const T* aSrc,
                                        size_t aCount)
  {
    copyAndSwapTo<ThisEndian, Little>(aDest, aSrc, aCount);
  }

  /*
   * Likewise, but converts values in place.
   */
  template<typename T>
  static void swapToLittleEndianInPlace(T* aPtr, size_t aCount)
  {
    maybeSwapInPlace<ThisEndian, Little>(aPtr, aCount);
  }

  /*
   * Converts a value of type T to big-endian format.
   */
  template<typename T>
  MOZ_WARN_UNUSED_RESULT static T swapToBigEndian(T aValue)
  {
    return maybeSwap<ThisEndian, Big>(aValue);
  }

  /*
   * Copies |aCount| values of type T starting at |aSrc| to |aDest|, converting
   * them to big-endian format if ThisEndian is Little.
   * As with memcpy, |aDest| and |aSrc| must not overlap.
   */
  template<typename T>
  static void copyAndSwapToBigEndian(void* aDest, const T* aSrc,
                                     size_t aCount)
  {
    copyAndSwapTo<ThisEndian, Big>(aDest, aSrc, aCount);
  }

  /*
   * Likewise, but converts values in place.
   */
  template<typename T>
  static void swapToBigEndianInPlace(T* aPtr, size_t aCount)
  {
    maybeSwapInPlace<ThisEndian, Big>(aPtr, aCount);
  }

  /*
   * Synonyms for the big-endian functions, for better readability
   * in network code.
   */

  template<typename T>
  MOZ_WARN_UNUSED_RESULT static T swapToNetworkOrder(T aValue)
  {
    return swapToBigEndian(aValue);
  }

  template<typename T>
  static void
  copyAndSwapToNetworkOrder(void* aDest, const T* aSrc, size_t aCount)
  {
    copyAndSwapToBigEndian(aDest, aSrc, aCount);
  }

  template<typename T>
  static void
  swapToNetworkOrderInPlace(T* aPtr, size_t aCount)
  {
    swapToBigEndianInPlace(aPtr, aCount);
  }

  /*
   * Converts a value of type T from little-endian format.
   */
  template<typename T>
  MOZ_WARN_UNUSED_RESULT static T swapFromLittleEndian(T aValue)
  {
    return maybeSwap<Little, ThisEndian>(aValue);
  }

  /*
   * Copies |aCount| values of type T starting at |aSrc| to |aDest|, converting
   * them to little-endian format if ThisEndian is Big.
   * As with memcpy, |aDest| and |aSrc| must not overlap.
   */
  template<typename T>
  static void copyAndSwapFromLittleEndian(T* aDest, const void* aSrc,
                                          size_t aCount)
  {
    copyAndSwapFrom<Little, ThisEndian>(aDest, aSrc, aCount);
  }

  /*
   * Likewise, but converts values in place.
   */
  template<typename T>
  static void swapFromLittleEndianInPlace(T* aPtr, size_t aCount)
  {
    maybeSwapInPlace<Little, ThisEndian>(aPtr, aCount);
  }

  /*
   * Converts a value of type T from big-endian format.
   */
  template<typename T>
  MOZ_WARN_UNUSED_RESULT static T swapFromBigEndian(T aValue)
  {
    return maybeSwap<Big, ThisEndian>(aValue);
  }

  /*
   * Copies |aCount| values of type T starting at |aSrc| to |aDest|, converting
   * them to big-endian format if ThisEndian is Little.
   * As with memcpy, |aDest| and |aSrc| must not overlap.
   */
  template<typename T>
  static void copyAndSwapFromBigEndian(T* aDest, const void* aSrc,
                                       size_t aCount)
  {
    copyAndSwapFrom<Big, ThisEndian>(aDest, aSrc, aCount);
  }

  /*
   * Likewise, but converts values in place.
   */
  template<typename T>
  static void swapFromBigEndianInPlace(T* aPtr, size_t aCount)
  {
    maybeSwapInPlace<Big, ThisEndian>(aPtr, aCount);
  }

  /*
   * Synonyms for the big-endian functions, for better readability
   * in network code.
   */
  template<typename T>
  MOZ_WARN_UNUSED_RESULT static T swapFromNetworkOrder(T aValue)
  {
    return swapFromBigEndian(aValue);
  }

  template<typename T>
  static void copyAndSwapFromNetworkOrder(T* aDest, const void* aSrc,
                                          size_t aCount)
  {
    copyAndSwapFromBigEndian(aDest, aSrc, aCount);
  }

  template<typename T>
  static void swapFromNetworkOrderInPlace(T* aPtr, size_t aCount)
  {
    swapFromBigEndianInPlace(aPtr, aCount);
  }

private:
  /**
   * Read a value of type T, encoded in endianness ThisEndian from |aPtr|.
   * Return that value encoded in native endianness.
   */
  template<typename T>
  static T read(const void* aPtr)
  {
    union
    {
      T mVal;
      uint8_t mBuffer[sizeof(T)];
    } u;
    memcpy(u.mBuffer, aPtr, sizeof(T));
    return maybeSwap<ThisEndian, MOZ_NATIVE_ENDIANNESS>(u.mVal);
  }

  /**
   * Write a value of type T, in native endianness, to |aPtr|, in ThisEndian
   * endianness.
   */
  template<typename T>
  static void write(void* aPtr, T aValue)
  {
    T tmp = maybeSwap<MOZ_NATIVE_ENDIANNESS, ThisEndian>(aValue);
    memcpy(aPtr, &tmp, sizeof(T));
  }

  Endian() = delete;
  Endian(const Endian& aTther) = delete;
  void operator=(const Endian& aOther) = delete;
};

template<Endianness ThisEndian>
class EndianReadWrite : public Endian<ThisEndian>
{
private:
  typedef Endian<ThisEndian> super;

public:
  using super::readUint16;
  using super::readUint32;
  using super::readUint64;
  using super::readInt16;
  using super::readInt32;
  using super::readInt64;
  using super::writeUint16;
  using super::writeUint32;
  using super::writeUint64;
  using super::writeInt16;
  using super::writeInt32;
  using super::writeInt64;
};

} /* namespace detail */

class LittleEndian final : public detail::EndianReadWrite<detail::Little>
{};

class BigEndian final : public detail::EndianReadWrite<detail::Big>
{};

typedef BigEndian NetworkEndian;

class NativeEndian final : public detail::Endian<MOZ_NATIVE_ENDIANNESS>
{
private:
  typedef detail::Endian<MOZ_NATIVE_ENDIANNESS> super;

public:
  /*
   * These functions are intended for cases where you have data in your
   * native-endian format and you need the data to appear in the appropriate
   * endianness for transmission, serialization, etc.
   */
  using super::swapToLittleEndian;
  using super::copyAndSwapToLittleEndian;
  using super::swapToLittleEndianInPlace;
  using super::swapToBigEndian;
  using super::copyAndSwapToBigEndian;
  using super::swapToBigEndianInPlace;
  using super::swapToNetworkOrder;
  using super::copyAndSwapToNetworkOrder;
  using super::swapToNetworkOrderInPlace;

  /*
   * These functions are intended for cases where you have data in the
   * given endianness (e.g. reading from disk or a file-format) and you
   * need the data to appear in native-endian format for processing.
   */
  using super::swapFromLittleEndian;
  using super::copyAndSwapFromLittleEndian;
  using super::swapFromLittleEndianInPlace;
  using super::swapFromBigEndian;
  using super::copyAndSwapFromBigEndian;
  using super::swapFromBigEndianInPlace;
  using super::swapFromNetworkOrder;
  using super::copyAndSwapFromNetworkOrder;
  using super::swapFromNetworkOrderInPlace;
};

#undef MOZ_NATIVE_ENDIANNESS

} /* namespace mozilla */

#endif /* mozilla_Endian_h */
