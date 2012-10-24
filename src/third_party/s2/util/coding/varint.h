// Copyright 2001 and onwards Google Inc.
//
// Raw support for varint encoding.  Higher level interfaces are
// provided by Encoder/Decoder/IOBuffer.  Clients should typically use
// those interfaces, unless speed is paramount.
//
// If decoding speed is very important, consider using PrefixVarint instead.
// It has the same compression ratio, but generally faster decoding.
//
// Provided routines:
//      vi_parse32_unchecked
//      vi_parse64_unchecked
//      vi_encode32_unchecked
//      vi_encode64_unchecked

#ifndef _VARINT_H
#define _VARINT_H

#include <string>
using std::string;

#include "base/basictypes.h"

// Just a namespace, not a real class
class Varint {
 public:
  // Maximum lengths of varint encoding of uint32 and uint64
  static const int kMax32; // = 5;
  static const int kMax64;  // = 10;

  // REQUIRES   "ptr" points to a buffer of length at least kMaxXX
  // EFFECTS    Scan next varint from "ptr" and store in OUTPUT.
  //            Returns pointer just past last read byte.  Returns
  //            NULL if a valid varint value was not found.
  static const char* Parse32(const char* ptr, uint32* OUTPUT);
  static const char* Parse64(const char* ptr, uint64* OUTPUT);

  // A fully inlined version of Parse32: useful in the most time critical
  // routines, but its code size is large
  static const char* Parse32Inline(const char* ptr, uint32* OUTPUT);

  // REQUIRES   "ptr" points just past the last byte of a varint-encoded value.
  // REQUIRES   A second varint must be encoded just before the one we parse,
  //            OR "base" must point to the first byte of the one we parse.
  // REQUIRES   Bytes [base, ptr-1] are readable
  //
  // EFFECTS    Scan backwards from "ptr" and store in OUTPUT. Stop at the last
  //            byte of the previous varint, OR at "base", whichever one comes
  //            first. Returns pointer to the first byte of the decoded varint
  //            NULL if a valid varint value was not found.
  static const char* Parse32Backward(const char* ptr, const char* base,
                                     uint32* OUTPUT);
  static const char* Parse64Backward(const char* ptr, const char* base,
                                     uint64* OUTPUT);

  // Attempts to parse a varint32 from a prefix of the bytes in [ptr,limit-1].
  // Never reads a character at or beyond limit.  If a valid/terminated varint32
  // was found in the range, stores it in *OUTPUT and returns a pointer just
  // past the last byte of the varint32. Else returns NULL.  On success,
  // "result <= limit".
  static const char* Parse32WithLimit(const char* ptr, const char* limit,
                                      uint32* OUTPUT);
  static const char* Parse64WithLimit(const char* ptr, const char* limit,
                                      uint64* OUTPUT);

  // REQUIRES   "ptr" points to the first byte of a varint-encoded value.
  // EFFECTS     Scans until the end of the varint and returns a pointer just
  //             past the last byte. Returns NULL if "ptr" does not point to
  //             a valid varint value.
  static const char* Skip32(const char* ptr);
  static const char* Skip64(const char* ptr);

  // REQUIRES   "ptr" points just past the last byte of a varint-encoded value.
  // REQUIRES   A second varint must be encoded just before the one we parse,
  //            OR "base" must point to the first byte of the one we parse.
  // REQUIRES   Bytes [base, ptr-1] are readable
  //
  // EFFECTS    Scan backwards from "ptr" and stop at the last byte of the
  //            previous varint, OR at "base", whichever one comes first.
  //            Returns pointer to the first byte of the skipped varint or
  //            NULL if a valid varint value was not found.
  static const char* Skip32Backward(const char* ptr, const char* base);
  static const char* Skip64Backward(const char* ptr, const char* base);

  // REQUIRES   "ptr" points to a buffer of length sufficient to hold "v".
  // EFFECTS    Encodes "v" into "ptr" and returns a pointer to the
  //            byte just past the last encoded byte.
  static char* Encode32(char* ptr, uint32 v);
  static char* Encode64(char* ptr, uint64 v);

  // A fully inlined version of Encode32: useful in the most time critical
  // routines, but its code size is large
  static char* Encode32Inline(char* ptr, uint32 v);

  // EFFECTS    Returns the encoding length of the specified value.
  static int Length32(uint32 v);
  static int Length64(uint64 v);

  static int Length32NonInline(uint32 v);

  // EFFECTS    Appends the varint representation of "value" to "*s".
  static void Append32(string* s, uint32 value);
  static void Append64(string* s, uint64 value);

  // EFFECTS    Encodes a pair of values to "*s".  The encoding
  //            is done by weaving together 4 bit groups of
  //            each number into a single 64 bit value, and then
  //            encoding this value as a Varint64 value.  This means
  //            that if both a and b are small, both values can be
  //            encoded in a single byte.
  static void EncodeTwo32Values(string* s, uint32 a, uint32 b);
  static const char* DecodeTwo32Values(const char* ptr, uint32* a, uint32* b);

  // Decode and sum up a sequence of deltas until the sum >= goal.
  // It is significantly faster than calling ParseXXInline in a loop.
  // NOTE(user): The code does NO error checking, it assumes all the
  // deltas are valid and the sum of deltas will never exceed kint64max. The
  // code works for both 32bits and 64bits varint, and on 64 bits machines,
  // the 64 bits version is almost always faster. Thus we only have a 64 bits
  // interface here. The interface is slightly different from the other
  // functions in that it requires *signed* integers.
  // REQUIRES   "ptr" points to the first byte of a varint-encoded delta.
  //            The sum of deltas >= goal (the code does NO boundary check).
  //            goal is positive and fit into a signed int64.
  // EFFECTS    Returns a pointer just past last read byte.
  //            "out" stores the actual sum.
  static const char* FastDecodeDeltas(const char* ptr, int64 goal, int64* out);

 private:
  static const char* Parse32FallbackInline(const char* p, uint32* val);
  static const char* Parse32Fallback(const char* p, uint32* val);
  static const char* Parse64Fallback(const char* p, uint64* val);

  static char* Encode32Fallback(char* ptr, uint32 v);

  static const char* DecodeTwo32ValuesSlow(const char* p, uint32* a, uint32* b);
  static const char* Parse32BackwardSlow(const char* ptr, const char* base,
                                         uint32* OUTPUT);
  static const char* Parse64BackwardSlow(const char* ptr, const char* base,
                                         uint64* OUTPUT);
  static const char* Skip32BackwardSlow(const char* ptr, const char* base);
  static const char* Skip64BackwardSlow(const char* ptr, const char* base);

  static void Append32Slow(string* s, uint32 value);
  static void Append64Slow(string* s, uint64 value);

  // Mapping from rightmost bit set to the number of bytes required
  static const char length32_bytes_required[33];
};

/***** Implementation details; clients should ignore *****/

inline const char* Varint::Parse32FallbackInline(const char* p,
                                                 uint32* OUTPUT) {
  // Fast path
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(p);
  uint32 byte, result;
  byte = *(ptr++); result = byte & 127;
  assert(byte >= 128);   // Already checked in inlined prelude
  byte = *(ptr++); result |= (byte & 127) <<  7; if (byte < 128) goto done;
  byte = *(ptr++); result |= (byte & 127) << 14; if (byte < 128) goto done;
  byte = *(ptr++); result |= (byte & 127) << 21; if (byte < 128) goto done;
  byte = *(ptr++); result |= (byte & 127) << 28; if (byte < 128) goto done;
  return NULL;       // Value is too long to be a varint32
 done:
  *OUTPUT = result;
  return reinterpret_cast<const char*>(ptr);
}

inline const char* Varint::Parse32(const char* p, uint32* OUTPUT) {
  // Fast path for inlining
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(p);
  uint32 byte = *ptr;
  if (byte < 128) {
    *OUTPUT = byte;
    return reinterpret_cast<const char*>(ptr) + 1;
  } else {
    return Parse32Fallback(p, OUTPUT);
  }
}

inline const char* Varint::Parse32Inline(const char* p, uint32* OUTPUT) {
  // Fast path for inlining
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(p);
  uint32 byte = *ptr;
  if (byte < 128) {
    *OUTPUT = byte;
    return reinterpret_cast<const char*>(ptr) + 1;
  } else {
    return Parse32FallbackInline(p, OUTPUT);
  }
}

inline const char* Varint::Skip32(const char* p) {
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(p);
  if (*ptr++ < 128) return reinterpret_cast<const char*>(ptr);
  if (*ptr++ < 128) return reinterpret_cast<const char*>(ptr);
  if (*ptr++ < 128) return reinterpret_cast<const char*>(ptr);
  if (*ptr++ < 128) return reinterpret_cast<const char*>(ptr);
  if (*ptr++ < 128) return reinterpret_cast<const char*>(ptr);
  return NULL; // value is too long to be a varint32
}

inline const char* Varint::Parse32Backward(const char* p, const char* base,
                                           uint32* OUTPUT) {
  if (p > base + kMax32) {
    // Fast path
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(p);
    uint32 byte, result;
    byte = *(--ptr); if (byte > 127) return NULL;
    result = byte;
    byte = *(--ptr); if (byte < 128) goto done;
    result <<= 7; result |= (byte & 127);
    byte = *(--ptr); if (byte < 128) goto done;
    result <<= 7; result |= (byte & 127);
    byte = *(--ptr); if (byte < 128) goto done;
    result <<= 7; result |= (byte & 127);
    byte = *(--ptr); if (byte < 128) goto done;
    result <<= 7; result |= (byte & 127);
    byte = *(--ptr); if (byte < 128) goto done;
    return NULL; // Value is too long to be a varint32
 done:
    *OUTPUT = result;
    return reinterpret_cast<const char*>(ptr+1);
  } else {
    return Parse32BackwardSlow(p, base, OUTPUT);
  }
}

inline const char* Varint::Skip32Backward(const char* p, const char* base) {
  if (p > base + kMax32) {
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(p);
    if (*(--ptr) > 127) return NULL;
    if (*(--ptr) < 128) return reinterpret_cast<const char*>(ptr+1);
    if (*(--ptr) < 128) return reinterpret_cast<const char*>(ptr+1);
    if (*(--ptr) < 128) return reinterpret_cast<const char*>(ptr+1);
    if (*(--ptr) < 128) return reinterpret_cast<const char*>(ptr+1);
    if (*(--ptr) < 128) return reinterpret_cast<const char*>(ptr+1);
    return NULL; // value is too long to be a varint32
  } else {
    return Skip32BackwardSlow(p, base);
  }
}

inline const char* Varint::Parse32WithLimit(const char* p,
                                            const char* l,
                                            uint32* OUTPUT) {
  if (p + kMax32 <= l) {
    return Parse32(p, OUTPUT);
  } else {
    // Slow version with bounds checks
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(p);
    const unsigned char* limit = reinterpret_cast<const unsigned char*>(l);
    uint32 b, result;
    if (ptr >= limit) return NULL;
    b = *(ptr++); result = b & 127;          if (b < 128) goto done;
    if (ptr >= limit) return NULL;
    b = *(ptr++); result |= (b & 127) <<  7; if (b < 128) goto done;
    if (ptr >= limit) return NULL;
    b = *(ptr++); result |= (b & 127) << 14; if (b < 128) goto done;
    if (ptr >= limit) return NULL;
    b = *(ptr++); result |= (b & 127) << 21; if (b < 128) goto done;
    if (ptr >= limit) return NULL;
    b = *(ptr++); result |= (b & 127) << 28; if (b < 16) goto done;
    return NULL;       // Value is too long to be a varint32
   done:
    *OUTPUT = result;
    return reinterpret_cast<const char*>(ptr);
  }

}

inline const char* Varint::Parse64(const char* p, uint64* OUTPUT) {
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(p);
  uint32 byte = *ptr;
  if (byte < 128) {
    *OUTPUT = byte;
    return reinterpret_cast<const char*>(ptr) + 1;
  } else {
    return Parse64Fallback(p, OUTPUT);
  }
}

inline const char* Varint::Skip64(const char* p) {
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(p);
  if (*ptr++ < 128) return reinterpret_cast<const char*>(ptr);
  if (*ptr++ < 128) return reinterpret_cast<const char*>(ptr);
  if (*ptr++ < 128) return reinterpret_cast<const char*>(ptr);
  if (*ptr++ < 128) return reinterpret_cast<const char*>(ptr);
  if (*ptr++ < 128) return reinterpret_cast<const char*>(ptr);
  if (*ptr++ < 128) return reinterpret_cast<const char*>(ptr);
  if (*ptr++ < 128) return reinterpret_cast<const char*>(ptr);
  if (*ptr++ < 128) return reinterpret_cast<const char*>(ptr);
  if (*ptr++ < 128) return reinterpret_cast<const char*>(ptr);
  if (*ptr++ < 128) return reinterpret_cast<const char*>(ptr);
  return NULL; // value is too long to be a varint64
}

inline const char* Varint::Parse64Backward(const char* p, const char* b,
                                           uint64* OUTPUT) {
  if (p > b + kMax64) {
    // Fast path
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(p);
    uint32 byte;
    uint64 res;

    byte = *(--ptr); if (byte > 127) return NULL;

    res = byte;
    byte = *(--ptr); if (byte < 128) goto done;
    res <<= 7; res |= (byte & 127);
    byte = *(--ptr); if (byte < 128) goto done;
    res <<= 7; res |= (byte & 127);
    byte = *(--ptr); if (byte < 128) goto done;
    res <<= 7; res |= (byte & 127);
    byte = *(--ptr); if (byte < 128) goto done;
    res <<= 7; res |= (byte & 127);
    byte = *(--ptr); if (byte < 128) goto done;
    res <<= 7; res |= (byte & 127);
    byte = *(--ptr); if (byte < 128) goto done;
    res <<= 7; res |= (byte & 127);
    byte = *(--ptr); if (byte < 128) goto done;
    res <<= 7; res |= (byte & 127);
    byte = *(--ptr); if (byte < 128) goto done;
    res <<= 7; res |= (byte & 127);
    byte = *(--ptr); if (byte < 128) goto done;
    res <<= 7; res |= (byte & 127);
    byte = *(--ptr); if (byte < 128) goto done;

    return NULL;       // Value is too long to be a varint64

 done:
    *OUTPUT = res;
    return reinterpret_cast<const char*>(ptr + 1);
  } else {
    return Parse64BackwardSlow(p, b, OUTPUT);
  }
}

inline const char* Varint::Skip64Backward(const char* p, const char* b) {
  if (p > b + kMax64) {
    // Fast path
    const unsigned char* ptr = reinterpret_cast<const unsigned char*>(p);
    if (*(--ptr) > 127) return NULL;
    if (*(--ptr) < 128) return reinterpret_cast<const char*>(ptr+1);
    if (*(--ptr) < 128) return reinterpret_cast<const char*>(ptr+1);
    if (*(--ptr) < 128) return reinterpret_cast<const char*>(ptr+1);
    if (*(--ptr) < 128) return reinterpret_cast<const char*>(ptr+1);
    if (*(--ptr) < 128) return reinterpret_cast<const char*>(ptr+1);
    if (*(--ptr) < 128) return reinterpret_cast<const char*>(ptr+1);
    if (*(--ptr) < 128) return reinterpret_cast<const char*>(ptr+1);
    if (*(--ptr) < 128) return reinterpret_cast<const char*>(ptr+1);
    if (*(--ptr) < 128) return reinterpret_cast<const char*>(ptr+1);
    if (*(--ptr) < 128) return reinterpret_cast<const char*>(ptr+1);
    return NULL; // value is too long to be a varint64
  } else {
    return Skip64BackwardSlow(p, b);
  }
}

inline const char* Varint::DecodeTwo32Values(const char* p,
                                             uint32* a, uint32* b) {
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(p);
  if (*ptr < 128) {
    // Special case for small values
    *a = (*ptr & 0xf);
    *b = *ptr >> 4;
    return reinterpret_cast<const char*>(ptr) + 1;
  } else {
    return DecodeTwo32ValuesSlow(p, a, b);
  }
}

#if (defined __i386__ || defined __x86_64__) && defined __GNUC__
inline int Varint::Length32(uint32 v) {
  // Find the rightmost bit set, and index into a small table

  // "ro" for the input spec means the input can come from either a
  // register ("r") or offsetable memory ("o").
  //
  // If "n == 0", the "bsr" instruction sets the "Z" flag, so we
  // conditionally move "-1" into the result.
  //
  // Note: the cmovz was introduced on PIII's, and may not work on
  // older machines.
  int bits;
  const int neg1 = -1;
  __asm__("bsr   %1, %0\n\t"
          "cmovz %2, %0"
          : "=&r" (bits)               // Output spec, early clobber
          : "ro" (v), "r" (neg1)        // Input spec
          : "cc"                        // Clobbers condition-codes
          );
  return Varint::length32_bytes_required[bits+1];
}

#else
inline int Varint::Length32(uint32 v) {
  /*
    The following version is about 1.5X the code size, but is faster than
    the loop below.

  if (v < (1u << 14)) {
    if (v < (1u << 7)) {
      return 1;
    } else {
      return 2;
    }
  } else {
    if (v < (1u << 28)) {
      if (v < (1u << 21)) {
        return 3;
      } else {
        return 4;
      }
    } else {
      return 5;
    }
  }
  */

  // Each byte of output stores 7 bits of "v" until "v" becomes zero
  int nbytes = 0;
  do {
    nbytes++;
    v >>= 7;
  } while (v != 0);
  return nbytes;
}
#endif

inline void Varint::Append32(string* s, uint32 value) {
  // Inline the fast-path for single-character output, but fall back to the .cc
  // file for the full version. The size<capacity check is so the compiler can
  // optimize out the string resize code.
  if (value < 128 && s->size() < s->capacity()) {
    s->push_back((unsigned char)value);
  } else {
    Append32Slow(s, value);
  }
}

inline void Varint::Append64(string* s, uint64 value) {
  // Inline the fast-path for single-character output, but fall back to the .cc
  // file for the full version. The size<capacity check is so the compiler can
  // optimize out the string resize code.
  if (value < 128 && s->size() < s->capacity()) {
    s->push_back((unsigned char)value);
  } else {
    Append64Slow(s, value);
  }
}

inline char* Varint::Encode32Inline(char* sptr, uint32 v) {
  // Operate on characters as unsigneds
  unsigned char* ptr = reinterpret_cast<unsigned char*>(sptr);
  static const int B = 128;
  if (v < (1<<7)) {
    *(ptr++) = v;
  } else if (v < (1<<14)) {
    *(ptr++) = v | B;
    *(ptr++) = v>>7;
  } else if (v < (1<<21)) {
    *(ptr++) = v | B;
    *(ptr++) = (v>>7) | B;
    *(ptr++) = v>>14;
  } else if (v < (1<<28)) {
    *(ptr++) = v | B;
    *(ptr++) = (v>>7) | B;
    *(ptr++) = (v>>14) | B;
    *(ptr++) = v>>21;
  } else {
    *(ptr++) = v | B;
    *(ptr++) = (v>>7) | B;
    *(ptr++) = (v>>14) | B;
    *(ptr++) = (v>>21) | B;
    *(ptr++) = v>>28;
  }
  return reinterpret_cast<char*>(ptr);
}

#if (-1 >> 1) != -1
#error FastDecodeDeltas() needs right-shift to sign-extend.
#endif
inline const char* Varint::FastDecodeDeltas(const char* ptr,
                                            int64 goal,
                                            int64* out) {
  int64 value;
  int64 sum = - goal;
  int64 shift = 0;
  // Make decoding faster by eliminating unpredictable branching.
  do {
    value = static_cast<int8>(*ptr++);  // sign extend one byte of data
    sum += (value & 0x7F) << shift;
    shift += 7;
    // (value >> 7) is either -1(continuation byte) or 0 (stop byte)
    shift &= value >> 7;
    // Loop if we haven't reached goal (sum < 0) or we haven't finished
    // parsing current delta (value < 0). We write it in the form of
    // (a | b) < 0 as opposed to (a < 0 || b < 0) as the former one is
    // usually as fast as a test for (a < 0).
  } while ((sum | value) < 0);

  *out = goal + sum;
  return ptr;
}

#endif /* _VARINT_H */
