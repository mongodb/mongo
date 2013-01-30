// Copyright 2005 Google, Inc
//
// Utility functions that depend on bytesex. We define htonll and ntohll,
// as well as "Google" versions of all the standards: ghtonl, ghtons, and
// so on. These functions do exactly the same as their standard variants,
// but don't require including the dangerous netinet/in.h.

#ifndef UTIL_ENDIAN_ENDIAN_H_
#define UTIL_ENDIAN_ENDIAN_H_

#include "base/integral_types.h"
#include "base/logging.h"
#include "base/port.h"
#include "base/int128.h"

inline uint64 gbswap_64(uint64 host_int) {
#if defined(COMPILER_GCC3) && defined(__x86_64__)
  // Adapted from /usr/include/byteswap.h.
  if (__builtin_constant_p(host_int)) {
    return __bswap_constant_64(host_int);
  } else {
    register uint64 result;
    __asm__ ("bswap %0" : "=r" (result) : "0" (host_int));
    return result;
  }
#elif defined(bswap_64)
  return bswap_64(host_int);
#else
  return static_cast<uint64>(bswap_32(static_cast<uint32>(host_int >> 32))) |
    (static_cast<uint64>(bswap_32(static_cast<uint32>(host_int))) << 32);
#endif  // bswap_64
}

#ifdef IS_LITTLE_ENDIAN

// Definitions for ntohl etc. that don't require us to include
// netinet/in.h. We wrap bswap_32 and bswap_16 in functions rather
// than just #defining them because in debug mode, gcc doesn't
// correctly handle the (rather involved) definitions of bswap_32.
// gcc guarantees that inline functions are as fast as macros, so
// this isn't a performance hit.
inline uint16 ghtons(uint16 x) { return bswap_16(x); }
inline uint32 ghtonl(uint32 x) { return bswap_32(x); }
inline uint64 ghtonll(uint64 x) { return gbswap_64(x); }

#elif defined IS_BIG_ENDIAN

// These definitions are a lot simpler on big-endian machines
#define ghtons(x) (x)
#define ghtonl(x) (x)
#define ghtonll(x) (x)

#else
#error "Unsupported bytesex: Either IS_BIG_ENDIAN or IS_LITTLE_ENDIAN must be defined"
#endif  // bytesex

// Convert to little-endian storage, opposite of network format.
// Convert x from host to little endian: x = LittleEndian.FromHost(x);
// convert x from little endian to host: x = LittleEndian.ToHost(x);
//
//  Store values into unaligned memory converting to little endian order:
//    LittleEndian.Store16(p, x);
//
//  Load unaligned values stored in little endian coverting to host order:
//    x = LittleEndian.Load16(p);
class LittleEndian {
 public:
  // Conversion functions.
#ifdef IS_LITTLE_ENDIAN

  static uint16 FromHost16(uint16 x) { return x; }
  static uint16 ToHost16(uint16 x) { return x; }

  static uint32 FromHost32(uint32 x) { return x; }
  static uint32 ToHost32(uint32 x) { return x; }

  static uint64 FromHost64(uint64 x) { return x; }
  static uint64 ToHost64(uint64 x) { return x; }

  static bool IsLittleEndian() { return true; }

#elif defined IS_BIG_ENDIAN

  static uint16 FromHost16(uint16 x) { return bswap_16(x); }
  static uint16 ToHost16(uint16 x) { return bswap_16(x); }

  static uint32 FromHost32(uint32 x) { return bswap_32(x); }
  static uint32 ToHost32(uint32 x) { return bswap_32(x); }

  static uint64 FromHost64(uint64 x) { return gbswap_64(x); }
  static uint64 ToHost64(uint64 x) { return gbswap_64(x); }

  static bool IsLittleEndian() { return false; }

#endif /* ENDIAN */

  // Functions to do unaligned loads and stores in little-endian order.
  static uint16 Load16(const void *p) {
    return ToHost16(UNALIGNED_LOAD16(p));
  }

  static void Store16(void *p, uint16 v) {
    UNALIGNED_STORE16(p, FromHost16(v));
  }

  static uint32 Load32(const void *p) {
    return ToHost32(UNALIGNED_LOAD32(p));
  }

  static void Store32(void *p, uint32 v) {
    UNALIGNED_STORE32(p, FromHost32(v));
  }

  static uint64 Load64(const void *p) {
    return ToHost64(UNALIGNED_LOAD64(p));
  }

  // Build a uint64 from 1-8 bytes.
  // 8 * len least significant bits are loaded from the memory with
  // LittleEndian order. The 64 - 8 * len most significant bits are
  // set all to 0.
  // In latex-friendly words, this function returns:
  //     $\sum_{i=0}^{len-1} p[i] 256^{i}$, where p[i] is unsigned.
  //
  // This function is equivalent with:
  // uint64 val = 0;
  // memcpy(&val, p, len);
  // return ToHost64(val);
  // TODO(user): write a small benchmark and benchmark the speed
  // of a memcpy based approach.
  //
  // For speed reasons this function does not work for len == 0.
  // The caller needs to guarantee that 1 <= len <= 8.
  static uint64 Load64VariableLength(const void * const p, int len) {
    DCHECK_GE(len, 1);
    DCHECK_LE(len, 8);
    const char * const buf = static_cast<const char * const>(p);
    uint64 val = 0;
    --len;
    do {
      val = (val << 8) | buf[len];
      // (--len >= 0) is about 10 % faster than (len--) in some benchmarks.
    } while (--len >= 0);
    // No ToHost64(...) needed. The bytes are accessed in little-endian manner
    // on every architecture.
    return val;
  }

  static void Store64(void *p, uint64 v) {
    UNALIGNED_STORE64(p, FromHost64(v));
  }

  static uint128 Load128(const void *p) {
    return uint128(
        ToHost64(UNALIGNED_LOAD64(reinterpret_cast<const uint64 *>(p) + 1)),
        ToHost64(UNALIGNED_LOAD64(p)));
  }

  static void Store128(void *p, const uint128 v) {
    UNALIGNED_STORE64(p, FromHost64(Uint128Low64(v)));
    UNALIGNED_STORE64(reinterpret_cast<uint64 *>(p) + 1,
                      FromHost64(Uint128High64(v)));
  }

  // Build a uint128 from 1-16 bytes.
  // 8 * len least significant bits are loaded from the memory with
  // LittleEndian order. The 128 - 8 * len most significant bits are
  // set all to 0.
  static uint128 Load128VariableLength(const void *p, int len) {
    if (len <= 8) {
      return uint128(Load64VariableLength(p, len));
    } else {
      return uint128(
          Load64VariableLength(static_cast<const char *>(p) + 8, len - 8),
          Load64(p));
    }
  }
};


// This one is safe to take as it's an extension
#define htonll(x) ghtonll(x)

// ntoh* and hton* are the same thing for any size and bytesex,
// since the function is an involution, i.e., its own inverse.
#define gntohl(x) ghtonl(x)
#define gntohs(x) ghtons(x)
#define gntohll(x) ghtonll(x)
#define ntohll(x) htonll(x)

#endif  // UTIL_ENDIAN_ENDIAN_H_
