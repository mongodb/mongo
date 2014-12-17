// Copyright (C) 1999 and onwards Google, Inc.
//
//
// This file contains the core of Bob Jenkins lookup2 algorithm.
//
// This file contains the basic hash "mix" code which is widely referenced.
// 
// This file also contains routines used to load an unaligned little-endian
// word from memory.  This relatively generic functionality probably
// shouldn't live in this file.

#ifndef UTIL_HASH_JENKINS_LOOKUP2_H__
#define UTIL_HASH_JENKINS_LOOKUP2_H__

#include "base/port.h"

// ----------------------------------------------------------------------
// mix()
//    The hash function I use is due to Bob Jenkins (see
//    http://burtleburtle.net/bob/hash/index.html).
//    Each mix takes 36 instructions, in 18 cycles if you're lucky.
//
//    On x86 architectures, this requires 45 instructions in 27 cycles,
//    if you're lucky.
// ----------------------------------------------------------------------

static inline void mix(uint32& a, uint32& b, uint32& c) {     // 32bit version
  a -= b; a -= c; a ^= (c>>13);
  b -= c; b -= a; b ^= (a<<8);
  c -= a; c -= b; c ^= (b>>13);
  a -= b; a -= c; a ^= (c>>12);
  b -= c; b -= a; b ^= (a<<16);
  c -= a; c -= b; c ^= (b>>5);
  a -= b; a -= c; a ^= (c>>3);
  b -= c; b -= a; b ^= (a<<10);
  c -= a; c -= b; c ^= (b>>15);
}

static inline void mix(uint64& a, uint64& b, uint64& c) {     // 64bit version
  a -= b; a -= c; a ^= (c>>43);
  b -= c; b -= a; b ^= (a<<9);
  c -= a; c -= b; c ^= (b>>8);
  a -= b; a -= c; a ^= (c>>38);
  b -= c; b -= a; b ^= (a<<23);
  c -= a; c -= b; c ^= (b>>5);
  a -= b; a -= c; a ^= (c>>35);
  b -= c; b -= a; b ^= (a<<49);
  c -= a; c -= b; c ^= (b>>11);
  a -= b; a -= c; a ^= (c>>12);
  b -= c; b -= a; b ^= (a<<18);
  c -= a; c -= b; c ^= (b>>22);
}


// Load an unaligned little endian word from memory.
//
// These routines are named Word32At(), Word64At() and Google1At().
// Long ago, the 32-bit version of this operation was implemented using
// signed characters.  The hash function that used this variant creates
// persistent hash values.  The hash routine needs to remain backwards
// compatible, so we renamed the word loading function 'Google1At' to
// make it clear this implements special functionality.
//
// If a machine has alignment constraints or is big endian, we must
// load the word a byte at a time.  Otherwise we can load the whole word
// from memory.
//
// [Plausibly, Word32At() and Word64At() should really be called
// UNALIGNED_LITTLE_ENDIAN_LOAD32() and UNALIGNED_LITTLE_ENDIAN_LOAD64()
// but that seems overly verbose.]

#if !defined(NEED_ALIGNED_LOADS) && defined(IS_LITTLE_ENDIAN)
static inline uint64 Word64At(const char *ptr) {
  return UNALIGNED_LOAD64(ptr);
}

static inline uint32 Word32At(const char *ptr) {
  return UNALIGNED_LOAD32(ptr);
}

// This produces the same results as the byte-by-byte version below.
// Here, we mask off the sign bits and subtract off two copies.  To
// see why this is the same as adding together the sign extensions,
// start by considering the low-order byte.  If we loaded an unsigned
// word and wanted to sign extend it, we isolate the sign bit and subtract
// that from zero which gives us a sequence of bits matching the sign bit
// at and above the sign bit.  If we remove (subtract) the sign bit and 
// add in the low order byte, we now have a sign-extended byte as desired.
// We can then operate on all four bytes in parallel because addition
// is associative and commutative.
//
// For example, consider sign extending the bytes 0x01 and 0x81.  For 0x01,
// the sign bit is zero, and 0x01 - 0 -0 = 1.  For 0x81, the sign bit is 1
// and we are computing 0x81 - 0x80 + (-0x80) == 0x01 + 0xFFFFFF80.
//
// Similarily, if we start with 0x8200 and want to sign extend that,
// we end up calculating 0x8200 - 0x8000 + (-0x8000) == 0xFFFF8000 + 0x0200
//
// Suppose we have two bytes at the same time.  Doesn't the adding of all
// those F's generate something wierd?  Ignore the F's and reassociate
// the addition.  For 0x8281, processing the bytes one at a time (like
// we used to do) calculates
//      [0x8200 - 0x8000 + (-0x8000)] + [0x0081 - 0x80 + (-0x80)]
//   == 0x8281 - 0x8080 - 0x8000 - 0x80
//   == 0x8281 - 0x8080 - 0x8080

static inline uint32 Google1At(const char *ptr) {
  uint32 t = UNALIGNED_LOAD32(ptr);
  uint32 masked = t & 0x80808080;
  return t - masked - masked;
}

#else

// NOTE:  This code is not normally used or tested.

static inline uint64 Word64At(const char *ptr) {
    return (static_cast<uint64>(ptr[0]) +
            (static_cast<uint64>(ptr[1]) << 8) +
            (static_cast<uint64>(ptr[2]) << 16) +
            (static_cast<uint64>(ptr[3]) << 24) +
            (static_cast<uint64>(ptr[4]) << 32) +
            (static_cast<uint64>(ptr[5]) << 40) +
            (static_cast<uint64>(ptr[6]) << 48) +
            (static_cast<uint64>(ptr[7]) << 56));
}

static inline uint32 Word32At(const char *ptr) {
    return (static_cast<uint32>(ptr[0]) +
            (static_cast<uint32>(ptr[1]) << 8) +
            (static_cast<uint32>(ptr[2]) << 16) +
            (static_cast<uint32>(ptr[3]) << 24));
}

static inline uint32 Google1At(const char *ptr2) {
  const schar * ptr = reinterpret_cast<const schar *>(ptr2);
  return (static_cast<schar>(ptr[0]) +
	  (static_cast<uint32>(ptr[1]) << 8) +
	  (static_cast<uint32>(ptr[2]) << 16) +
	  (static_cast<uint32>(ptr[3]) << 24));
}

#endif /* !NEED_ALIGNED_LOADS && IS_LITTLE_ENDIAN */

// Historically, WORD_HASH has always been defined as we always run on
// machines that don't NEED_ALIGNED_LOADS and which IS_LITTLE_ENDIAN.
//
// TODO(user): find occurences of WORD_HASH and adjust the code to
// use more meaningful concepts.
# define WORD_HASH

#endif  // UTIL_HASH_JENKINS_LOOKUP2_H__

