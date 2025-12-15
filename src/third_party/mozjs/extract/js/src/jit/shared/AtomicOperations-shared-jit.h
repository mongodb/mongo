/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* For overall documentation, see jit/AtomicOperations.h.
 *
 * NOTE CAREFULLY: This file is only applicable when we have configured a JIT
 * and the JIT is for the same architecture that we're compiling the shell for.
 * Simulators must use a different mechanism.
 *
 * See comments before the include nest near the end of jit/AtomicOperations.h
 * if you didn't understand that.
 */

#ifndef jit_shared_AtomicOperations_shared_jit_h
#define jit_shared_AtomicOperations_shared_jit_h

#include "mozilla/Assertions.h"

#include <stddef.h>
#include <stdint.h>

#include "jit/AtomicOperationsGenerated.h"
#include "vm/Float16.h"
#include "vm/Uint8Clamped.h"

namespace js {
namespace jit {

#ifndef JS_64BIT
// `AtomicCompilerFence` erects a reordering boundary for operations on the
// current thread.  We use it to prevent the compiler from reordering loads and
// stores inside larger primitives that are synthesized from cmpxchg.
extern void AtomicCompilerFence();
#endif

// `...MemcpyDown` moves bytes toward lower addresses in memory: dest <= src.
// `...MemcpyUp` moves bytes toward higher addresses in memory: dest >= src.
extern void AtomicMemcpyDownUnsynchronized(uint8_t* dest, const uint8_t* src,
                                           size_t nbytes);
extern void AtomicMemcpyUpUnsynchronized(uint8_t* dest, const uint8_t* src,
                                         size_t nbytes);

}  // namespace jit
}  // namespace js

inline bool js::jit::AtomicOperations::hasAtomic8() { return true; }

inline bool js::jit::AtomicOperations::isLockfree8() { return true; }

inline void js::jit::AtomicOperations::fenceSeqCst() { AtomicFenceSeqCst(); }

inline void js::jit::AtomicOperations::pause() { AtomicPause(); }

#define JIT_LOADOP(T, U, loadop)                   \
  template <>                                      \
  inline T AtomicOperations::loadSeqCst(T* addr) { \
    return (T)loadop((U*)addr);                    \
  }

#ifndef JS_64BIT
#  define JIT_LOADOP_CAS(T)                                   \
    template <>                                               \
    inline T AtomicOperations::loadSeqCst(T* addr) {          \
      AtomicCompilerFence();                                  \
      return (T)AtomicCmpXchg64SeqCst((uint64_t*)addr, 0, 0); \
    }
#endif  // !JS_64BIT

namespace js {
namespace jit {

JIT_LOADOP(int8_t, uint8_t, AtomicLoad8SeqCst)
JIT_LOADOP(uint8_t, uint8_t, AtomicLoad8SeqCst)
JIT_LOADOP(int16_t, uint16_t, AtomicLoad16SeqCst)
JIT_LOADOP(uint16_t, uint16_t, AtomicLoad16SeqCst)
JIT_LOADOP(int32_t, uint32_t, AtomicLoad32SeqCst)
JIT_LOADOP(uint32_t, uint32_t, AtomicLoad32SeqCst)

#ifdef JIT_LOADOP_CAS
JIT_LOADOP_CAS(int64_t)
JIT_LOADOP_CAS(uint64_t)
#else
JIT_LOADOP(int64_t, uint64_t, AtomicLoad64SeqCst)
JIT_LOADOP(uint64_t, uint64_t, AtomicLoad64SeqCst)
#endif

}  // namespace jit
}  // namespace js

#undef JIT_LOADOP
#undef JIT_LOADOP_CAS

#define JIT_STOREOP(T, U, storeop)                            \
  template <>                                                 \
  inline void AtomicOperations::storeSeqCst(T* addr, T val) { \
    storeop((U*)addr, val);                                   \
  }

#ifndef JS_64BIT
#  define JIT_STOREOP_CAS(T)                                                   \
    template <>                                                                \
    inline void AtomicOperations::storeSeqCst(T* addr, T val) {                \
      AtomicCompilerFence();                                                   \
      T oldval = *addr; /* good initial approximation */                       \
      for (;;) {                                                               \
        T nextval = (T)AtomicCmpXchg64SeqCst((uint64_t*)addr,                  \
                                             (uint64_t)oldval, (uint64_t)val); \
        if (nextval == oldval) {                                               \
          break;                                                               \
        }                                                                      \
        oldval = nextval;                                                      \
      }                                                                        \
      AtomicCompilerFence();                                                   \
    }
#endif  // !JS_64BIT

namespace js {
namespace jit {

JIT_STOREOP(int8_t, uint8_t, AtomicStore8SeqCst)
JIT_STOREOP(uint8_t, uint8_t, AtomicStore8SeqCst)
JIT_STOREOP(int16_t, uint16_t, AtomicStore16SeqCst)
JIT_STOREOP(uint16_t, uint16_t, AtomicStore16SeqCst)
JIT_STOREOP(int32_t, uint32_t, AtomicStore32SeqCst)
JIT_STOREOP(uint32_t, uint32_t, AtomicStore32SeqCst)

#ifdef JIT_STOREOP_CAS
JIT_STOREOP_CAS(int64_t)
JIT_STOREOP_CAS(uint64_t)
#else
JIT_STOREOP(int64_t, uint64_t, AtomicStore64SeqCst)
JIT_STOREOP(uint64_t, uint64_t, AtomicStore64SeqCst)
#endif

}  // namespace jit
}  // namespace js

#undef JIT_STOREOP
#undef JIT_STOREOP_CAS

#define JIT_EXCHANGEOP(T, U, xchgop)                          \
  template <>                                                 \
  inline T AtomicOperations::exchangeSeqCst(T* addr, T val) { \
    return (T)xchgop((U*)addr, (U)val);                       \
  }

#ifndef JS_64BIT
#  define JIT_EXCHANGEOP_CAS(T)                                                \
    template <>                                                                \
    inline T AtomicOperations::exchangeSeqCst(T* addr, T val) {                \
      AtomicCompilerFence();                                                   \
      T oldval = *addr;                                                        \
      for (;;) {                                                               \
        T nextval = (T)AtomicCmpXchg64SeqCst((uint64_t*)addr,                  \
                                             (uint64_t)oldval, (uint64_t)val); \
        if (nextval == oldval) {                                               \
          break;                                                               \
        }                                                                      \
        oldval = nextval;                                                      \
      }                                                                        \
      AtomicCompilerFence();                                                   \
      return oldval;                                                           \
    }
#endif  // !JS_64BIT

namespace js {
namespace jit {

JIT_EXCHANGEOP(int8_t, uint8_t, AtomicExchange8SeqCst)
JIT_EXCHANGEOP(uint8_t, uint8_t, AtomicExchange8SeqCst)
JIT_EXCHANGEOP(int16_t, uint16_t, AtomicExchange16SeqCst)
JIT_EXCHANGEOP(uint16_t, uint16_t, AtomicExchange16SeqCst)
JIT_EXCHANGEOP(int32_t, uint32_t, AtomicExchange32SeqCst)
JIT_EXCHANGEOP(uint32_t, uint32_t, AtomicExchange32SeqCst)

#ifdef JIT_EXCHANGEOP_CAS
JIT_EXCHANGEOP_CAS(int64_t)
JIT_EXCHANGEOP_CAS(uint64_t)
#else
JIT_EXCHANGEOP(int64_t, uint64_t, AtomicExchange64SeqCst)
JIT_EXCHANGEOP(uint64_t, uint64_t, AtomicExchange64SeqCst)
#endif

}  // namespace jit
}  // namespace js

#undef JIT_EXCHANGEOP
#undef JIT_EXCHANGEOP_CAS

#define JIT_CAS(T, U, cmpxchg)                                        \
  template <>                                                         \
  inline T AtomicOperations::compareExchangeSeqCst(T* addr, T oldval, \
                                                   T newval) {        \
    return (T)cmpxchg((U*)addr, (U)oldval, (U)newval);                \
  }

namespace js {
namespace jit {

JIT_CAS(int8_t, uint8_t, AtomicCmpXchg8SeqCst)
JIT_CAS(uint8_t, uint8_t, AtomicCmpXchg8SeqCst)
JIT_CAS(int16_t, uint16_t, AtomicCmpXchg16SeqCst)
JIT_CAS(uint16_t, uint16_t, AtomicCmpXchg16SeqCst)
JIT_CAS(int32_t, uint32_t, AtomicCmpXchg32SeqCst)
JIT_CAS(uint32_t, uint32_t, AtomicCmpXchg32SeqCst)
JIT_CAS(int64_t, uint64_t, AtomicCmpXchg64SeqCst)
JIT_CAS(uint64_t, uint64_t, AtomicCmpXchg64SeqCst)

}  // namespace jit
}  // namespace js

#undef JIT_CAS

#define JIT_FETCHADDOP(T, U, xadd)                            \
  template <>                                                 \
  inline T AtomicOperations::fetchAddSeqCst(T* addr, T val) { \
    return (T)xadd((U*)addr, (U)val);                         \
  }

#define JIT_FETCHSUBOP(T)                                     \
  template <>                                                 \
  inline T AtomicOperations::fetchSubSeqCst(T* addr, T val) { \
    return fetchAddSeqCst(addr, (T)(0 - val));                \
  }

#ifndef JS_64BIT
#  define JIT_FETCHADDOP_CAS(T)                                           \
    template <>                                                           \
    inline T AtomicOperations::fetchAddSeqCst(T* addr, T val) {           \
      AtomicCompilerFence();                                              \
      T oldval = *addr; /* Good initial approximation */                  \
      for (;;) {                                                          \
        T nextval = (T)AtomicCmpXchg64SeqCst(                             \
            (uint64_t*)addr, (uint64_t)oldval, (uint64_t)(oldval + val)); \
        if (nextval == oldval) {                                          \
          break;                                                          \
        }                                                                 \
        oldval = nextval;                                                 \
      }                                                                   \
      AtomicCompilerFence();                                              \
      return oldval;                                                      \
    }
#endif  // !JS_64BIT

namespace js {
namespace jit {

JIT_FETCHADDOP(int8_t, uint8_t, AtomicAdd8SeqCst)
JIT_FETCHADDOP(uint8_t, uint8_t, AtomicAdd8SeqCst)
JIT_FETCHADDOP(int16_t, uint16_t, AtomicAdd16SeqCst)
JIT_FETCHADDOP(uint16_t, uint16_t, AtomicAdd16SeqCst)
JIT_FETCHADDOP(int32_t, uint32_t, AtomicAdd32SeqCst)
JIT_FETCHADDOP(uint32_t, uint32_t, AtomicAdd32SeqCst)

#ifdef JIT_FETCHADDOP_CAS
JIT_FETCHADDOP_CAS(int64_t)
JIT_FETCHADDOP_CAS(uint64_t)
#else
JIT_FETCHADDOP(int64_t, uint64_t, AtomicAdd64SeqCst)
JIT_FETCHADDOP(uint64_t, uint64_t, AtomicAdd64SeqCst)
#endif

JIT_FETCHSUBOP(int8_t)
JIT_FETCHSUBOP(uint8_t)
JIT_FETCHSUBOP(int16_t)
JIT_FETCHSUBOP(uint16_t)
JIT_FETCHSUBOP(int32_t)
JIT_FETCHSUBOP(uint32_t)
JIT_FETCHSUBOP(int64_t)
JIT_FETCHSUBOP(uint64_t)

}  // namespace jit
}  // namespace js

#undef JIT_FETCHADDOP
#undef JIT_FETCHADDOP_CAS
#undef JIT_FETCHSUBOP

#define JIT_FETCHBITOPX(T, U, name, op)             \
  template <>                                       \
  inline T AtomicOperations::name(T* addr, T val) { \
    return (T)op((U*)addr, (U)val);                 \
  }

#define JIT_FETCHBITOP(T, U, andop, orop, xorop) \
  JIT_FETCHBITOPX(T, U, fetchAndSeqCst, andop)   \
  JIT_FETCHBITOPX(T, U, fetchOrSeqCst, orop)     \
  JIT_FETCHBITOPX(T, U, fetchXorSeqCst, xorop)

#ifndef JS_64BIT

#  define AND_OP &
#  define OR_OP |
#  define XOR_OP ^

#  define JIT_FETCHBITOPX_CAS(T, name, OP)                                 \
    template <>                                                            \
    inline T AtomicOperations::name(T* addr, T val) {                      \
      AtomicCompilerFence();                                               \
      T oldval = *addr;                                                    \
      for (;;) {                                                           \
        T nextval = (T)AtomicCmpXchg64SeqCst(                              \
            (uint64_t*)addr, (uint64_t)oldval, (uint64_t)(oldval OP val)); \
        if (nextval == oldval) {                                           \
          break;                                                           \
        }                                                                  \
        oldval = nextval;                                                  \
      }                                                                    \
      AtomicCompilerFence();                                               \
      return oldval;                                                       \
    }

#  define JIT_FETCHBITOP_CAS(T)                    \
    JIT_FETCHBITOPX_CAS(T, fetchAndSeqCst, AND_OP) \
    JIT_FETCHBITOPX_CAS(T, fetchOrSeqCst, OR_OP)   \
    JIT_FETCHBITOPX_CAS(T, fetchXorSeqCst, XOR_OP)

#endif  // !JS_64BIT

namespace js {
namespace jit {

JIT_FETCHBITOP(int8_t, uint8_t, AtomicAnd8SeqCst, AtomicOr8SeqCst,
               AtomicXor8SeqCst)
JIT_FETCHBITOP(uint8_t, uint8_t, AtomicAnd8SeqCst, AtomicOr8SeqCst,
               AtomicXor8SeqCst)
JIT_FETCHBITOP(int16_t, uint16_t, AtomicAnd16SeqCst, AtomicOr16SeqCst,
               AtomicXor16SeqCst)
JIT_FETCHBITOP(uint16_t, uint16_t, AtomicAnd16SeqCst, AtomicOr16SeqCst,
               AtomicXor16SeqCst)
JIT_FETCHBITOP(int32_t, uint32_t, AtomicAnd32SeqCst, AtomicOr32SeqCst,
               AtomicXor32SeqCst)
JIT_FETCHBITOP(uint32_t, uint32_t, AtomicAnd32SeqCst, AtomicOr32SeqCst,
               AtomicXor32SeqCst)

#ifdef JIT_FETCHBITOP_CAS
JIT_FETCHBITOP_CAS(int64_t)
JIT_FETCHBITOP_CAS(uint64_t)
#else
JIT_FETCHBITOP(int64_t, uint64_t, AtomicAnd64SeqCst, AtomicOr64SeqCst,
               AtomicXor64SeqCst)
JIT_FETCHBITOP(uint64_t, uint64_t, AtomicAnd64SeqCst, AtomicOr64SeqCst,
               AtomicXor64SeqCst)
#endif

}  // namespace jit
}  // namespace js

#undef JIT_FETCHBITOPX_CAS
#undef JIT_FETCHBITOPX
#undef JIT_FETCHBITOP_CAS
#undef JIT_FETCHBITOP

#define JIT_LOADSAFE(T, U, loadop)                                \
  template <>                                                     \
  inline T js::jit::AtomicOperations::loadSafeWhenRacy(T* addr) { \
    union {                                                       \
      U u;                                                        \
      T t;                                                        \
    };                                                            \
    u = loadop((U*)addr);                                         \
    return t;                                                     \
  }

#ifndef JS_64BIT
#  define JIT_LOADSAFE_TEARING(T)                                   \
    template <>                                                     \
    inline T js::jit::AtomicOperations::loadSafeWhenRacy(T* addr) { \
      MOZ_ASSERT(sizeof(T) == 8);                                   \
      union {                                                       \
        uint32_t u[2];                                              \
        T t;                                                        \
      };                                                            \
      uint32_t* ptr = (uint32_t*)addr;                              \
      u[0] = AtomicLoad32Unsynchronized(ptr);                       \
      u[1] = AtomicLoad32Unsynchronized(ptr + 1);                   \
      return t;                                                     \
    }
#endif  // !JS_64BIT

namespace js {
namespace jit {

JIT_LOADSAFE(int8_t, uint8_t, AtomicLoad8Unsynchronized)
JIT_LOADSAFE(uint8_t, uint8_t, AtomicLoad8Unsynchronized)
JIT_LOADSAFE(int16_t, uint16_t, AtomicLoad16Unsynchronized)
JIT_LOADSAFE(uint16_t, uint16_t, AtomicLoad16Unsynchronized)
JIT_LOADSAFE(int32_t, uint32_t, AtomicLoad32Unsynchronized)
JIT_LOADSAFE(uint32_t, uint32_t, AtomicLoad32Unsynchronized)
#ifdef JIT_LOADSAFE_TEARING
JIT_LOADSAFE_TEARING(int64_t)
JIT_LOADSAFE_TEARING(uint64_t)
JIT_LOADSAFE_TEARING(double)
#else
JIT_LOADSAFE(int64_t, uint64_t, AtomicLoad64Unsynchronized)
JIT_LOADSAFE(uint64_t, uint64_t, AtomicLoad64Unsynchronized)
JIT_LOADSAFE(double, uint64_t, AtomicLoad64Unsynchronized)
#endif
JIT_LOADSAFE(float, uint32_t, AtomicLoad32Unsynchronized)

// Clang requires a specialization for uint8_clamped.
template <>
inline uint8_clamped js::jit::AtomicOperations::loadSafeWhenRacy(
    uint8_clamped* addr) {
  return uint8_clamped(loadSafeWhenRacy((uint8_t*)addr));
}

// Clang requires a specialization for float16.
template <>
inline float16 js::jit::AtomicOperations::loadSafeWhenRacy(float16* addr) {
  return float16::fromRawBits(loadSafeWhenRacy((uint16_t*)addr));
}

}  // namespace jit
}  // namespace js

#undef JIT_LOADSAFE
#undef JIT_LOADSAFE_TEARING

#define JIT_STORESAFE(T, U, storeop)                                         \
  template <>                                                                \
  inline void js::jit::AtomicOperations::storeSafeWhenRacy(T* addr, T val) { \
    union {                                                                  \
      U u;                                                                   \
      T t;                                                                   \
    };                                                                       \
    t = val;                                                                 \
    storeop((U*)addr, u);                                                    \
  }

#ifndef JS_64BIT
#  define JIT_STORESAFE_TEARING(T)                                             \
    template <>                                                                \
    inline void js::jit::AtomicOperations::storeSafeWhenRacy(T* addr, T val) { \
      union {                                                                  \
        uint32_t u[2];                                                         \
        T t;                                                                   \
      };                                                                       \
      t = val;                                                                 \
      uint32_t* ptr = (uint32_t*)addr;                                         \
      AtomicStore32Unsynchronized(ptr, u[0]);                                  \
      AtomicStore32Unsynchronized(ptr + 1, u[1]);                              \
    }
#endif  // !JS_64BIT

namespace js {
namespace jit {

JIT_STORESAFE(int8_t, uint8_t, AtomicStore8Unsynchronized)
JIT_STORESAFE(uint8_t, uint8_t, AtomicStore8Unsynchronized)
JIT_STORESAFE(int16_t, uint16_t, AtomicStore16Unsynchronized)
JIT_STORESAFE(uint16_t, uint16_t, AtomicStore16Unsynchronized)
JIT_STORESAFE(int32_t, uint32_t, AtomicStore32Unsynchronized)
JIT_STORESAFE(uint32_t, uint32_t, AtomicStore32Unsynchronized)
#ifdef JIT_STORESAFE_TEARING
JIT_STORESAFE_TEARING(int64_t)
JIT_STORESAFE_TEARING(uint64_t)
JIT_STORESAFE_TEARING(double)
#else
JIT_STORESAFE(int64_t, uint64_t, AtomicStore64Unsynchronized)
JIT_STORESAFE(uint64_t, uint64_t, AtomicStore64Unsynchronized)
JIT_STORESAFE(double, uint64_t, AtomicStore64Unsynchronized)
#endif
JIT_STORESAFE(float, uint32_t, AtomicStore32Unsynchronized)

// Clang requires a specialization for uint8_clamped.
template <>
inline void js::jit::AtomicOperations::storeSafeWhenRacy(uint8_clamped* addr,
                                                         uint8_clamped val) {
  storeSafeWhenRacy((uint8_t*)addr, (uint8_t)val);
}

// Clang requires a specialization for float16.
template <>
inline void js::jit::AtomicOperations::storeSafeWhenRacy(float16* addr,
                                                         float16 val) {
  storeSafeWhenRacy((uint16_t*)addr, val.toRawBits());
}

}  // namespace jit
}  // namespace js

#undef JIT_STORESAFE
#undef JIT_STORESAFE_TEARING

void js::jit::AtomicOperations::memcpySafeWhenRacy(void* dest, const void* src,
                                                   size_t nbytes) {
  MOZ_ASSERT(!((char*)dest <= (char*)src && (char*)src < (char*)dest + nbytes));
  MOZ_ASSERT(!((char*)src <= (char*)dest && (char*)dest < (char*)src + nbytes));
  AtomicMemcpyDownUnsynchronized((uint8_t*)dest, (const uint8_t*)src, nbytes);
}

inline void js::jit::AtomicOperations::memmoveSafeWhenRacy(void* dest,
                                                           const void* src,
                                                           size_t nbytes) {
  if ((char*)dest <= (char*)src) {
    AtomicMemcpyDownUnsynchronized((uint8_t*)dest, (const uint8_t*)src, nbytes);
  } else {
    AtomicMemcpyUpUnsynchronized((uint8_t*)dest, (const uint8_t*)src, nbytes);
  }
}

#endif  // jit_shared_AtomicOperations_shared_jit_h
