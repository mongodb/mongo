/*
 * Copyright 2013-2014 MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <bson/bson-prelude.h>


#ifndef BSON_ATOMIC_H
#define BSON_ATOMIC_H


#include <bson/bson-config.h>
#include <bson/bson-compat.h>
#include <bson/bson-macros.h>

#ifdef _MSC_VER
#include <intrin.h>
#endif


BSON_BEGIN_DECLS

enum bson_memory_order {
   bson_memory_order_seq_cst,
   bson_memory_order_acquire,
   bson_memory_order_release,
   bson_memory_order_relaxed,
   bson_memory_order_acq_rel,
   bson_memory_order_consume,
};

#if defined(_M_ARM) /* MSVC memorder atomics are only avail on ARM */
#define MSVC_MEMORDER_SUFFIX(X) X
#else
#define MSVC_MEMORDER_SUFFIX(X)
#endif

#if defined(USE_LEGACY_GCC_ATOMICS) || (!defined(__clang__) && __GNUC__ == 4) || defined(__xlC__)
#define BSON_USE_LEGACY_GCC_ATOMICS
#else
#undef BSON_USE_LEGACY_GCC_ATOMICS
#endif

/* Not all GCC-like compilers support the current __atomic built-ins.  Older
 * GCC (pre-5) used different built-ins named with the __sync prefix.  When
 * compiling with such older GCC versions, it is necessary to use the applicable
 * functions, which requires redefining BSON_IF_GNU_LIKE and defining the
 * additional BSON_IF_GNU_LEGACY_ATOMICS macro here. */
#ifdef BSON_USE_LEGACY_GCC_ATOMICS
#undef BSON_IF_GNU_LIKE
#define BSON_IF_GNU_LIKE(...)
#define BSON_IF_MSVC(...)
#define BSON_IF_GNU_LEGACY_ATOMICS(...) __VA_ARGS__
#else
#define BSON_IF_GNU_LEGACY_ATOMICS(...)
#endif

/* CDRIVER-4229 zSeries with gcc 4.8.4 produces illegal instructions for int and
 * int32 atomic intrinsics. */
#if defined(__s390__) || defined(__s390x__) || defined(__zarch__)
#define BSON_EMULATE_INT32
#define BSON_EMULATE_INT
#endif

/* CDRIVER-4264 Contrary to documentation, VS 2013 targeting x86 does not
 * correctly/consistently provide _InterlockedPointerExchange. */
#if defined(_MSC_VER) && _MSC_VER < 1900 && defined(_M_IX86)
#define BSON_EMULATE_PTR
#endif

#define DEF_ATOMIC_OP(MSVC_Intrinsic, GNU_Intrinsic, GNU_Legacy_Intrinsic, Order, ...)                  \
   do {                                                                                                 \
      switch (Order) {                                                                                  \
      case bson_memory_order_acq_rel:                                                                   \
         BSON_IF_MSVC (return MSVC_Intrinsic (__VA_ARGS__);)                                            \
         BSON_IF_GNU_LIKE (return GNU_Intrinsic (__VA_ARGS__, __ATOMIC_ACQ_REL);)                       \
         BSON_IF_GNU_LEGACY_ATOMICS (return GNU_Legacy_Intrinsic (__VA_ARGS__);)                        \
      case bson_memory_order_seq_cst:                                                                   \
         BSON_IF_MSVC (return MSVC_Intrinsic (__VA_ARGS__);)                                            \
         BSON_IF_GNU_LIKE (return GNU_Intrinsic (__VA_ARGS__, __ATOMIC_SEQ_CST);)                       \
         BSON_IF_GNU_LEGACY_ATOMICS (return GNU_Legacy_Intrinsic (__VA_ARGS__);)                        \
      case bson_memory_order_acquire:                                                                   \
         BSON_IF_MSVC (return BSON_CONCAT (MSVC_Intrinsic, MSVC_MEMORDER_SUFFIX (_acq)) (__VA_ARGS__);) \
         BSON_IF_GNU_LIKE (return GNU_Intrinsic (__VA_ARGS__, __ATOMIC_ACQUIRE);)                       \
         BSON_IF_GNU_LEGACY_ATOMICS (return GNU_Legacy_Intrinsic (__VA_ARGS__);)                        \
      case bson_memory_order_consume:                                                                   \
         BSON_IF_MSVC (return BSON_CONCAT (MSVC_Intrinsic, MSVC_MEMORDER_SUFFIX (_acq)) (__VA_ARGS__);) \
         BSON_IF_GNU_LIKE (return GNU_Intrinsic (__VA_ARGS__, __ATOMIC_CONSUME);)                       \
         BSON_IF_GNU_LEGACY_ATOMICS (return GNU_Legacy_Intrinsic (__VA_ARGS__);)                        \
      case bson_memory_order_release:                                                                   \
         BSON_IF_MSVC (return BSON_CONCAT (MSVC_Intrinsic, MSVC_MEMORDER_SUFFIX (_rel)) (__VA_ARGS__);) \
         BSON_IF_GNU_LIKE (return GNU_Intrinsic (__VA_ARGS__, __ATOMIC_RELEASE);)                       \
         BSON_IF_GNU_LEGACY_ATOMICS (return GNU_Legacy_Intrinsic (__VA_ARGS__);)                        \
      case bson_memory_order_relaxed:                                                                   \
         BSON_IF_MSVC (return BSON_CONCAT (MSVC_Intrinsic, MSVC_MEMORDER_SUFFIX (_nf)) (__VA_ARGS__);)  \
         BSON_IF_GNU_LIKE (return GNU_Intrinsic (__VA_ARGS__, __ATOMIC_RELAXED);)                       \
         BSON_IF_GNU_LEGACY_ATOMICS (return GNU_Legacy_Intrinsic (__VA_ARGS__);)                        \
      default:                                                                                          \
         BSON_UNREACHABLE ("Invalid bson_memory_order value");                                          \
      }                                                                                                 \
   } while (0)


#define DEF_ATOMIC_CMPEXCH_STRONG(VCSuffix1, VCSuffix2, GNU_MemOrder, Ptr, ExpectActualVar, NewValue)    \
   do {                                                                                                  \
      BSON_IF_MSVC (ExpectActualVar = BSON_CONCAT3 (_InterlockedCompareExchange, VCSuffix1, VCSuffix2) ( \
                       Ptr, NewValue, ExpectActualVar);)                                                 \
      BSON_IF_GNU_LIKE ((void) __atomic_compare_exchange_n (Ptr,                                         \
                                                            &ExpectActualVar,                            \
                                                            NewValue,                                    \
                                                            false, /* Not weak */                        \
                                                            GNU_MemOrder,                                \
                                                            GNU_MemOrder);)                              \
      BSON_IF_GNU_LEGACY_ATOMICS (__typeof__ (ExpectActualVar) _val;                                     \
                                  _val = __sync_val_compare_and_swap (Ptr, ExpectActualVar, NewValue);   \
                                  ExpectActualVar = _val;)                                               \
   } while (0)


#define DEF_ATOMIC_CMPEXCH_WEAK(VCSuffix1, VCSuffix2, GNU_MemOrder, Ptr, ExpectActualVar, NewValue)      \
   do {                                                                                                  \
      BSON_IF_MSVC (ExpectActualVar = BSON_CONCAT3 (_InterlockedCompareExchange, VCSuffix1, VCSuffix2) ( \
                       Ptr, NewValue, ExpectActualVar);)                                                 \
      BSON_IF_GNU_LIKE ((void) __atomic_compare_exchange_n (Ptr,                                         \
                                                            &ExpectActualVar,                            \
                                                            NewValue,                                    \
                                                            true, /* Yes weak */                         \
                                                            GNU_MemOrder,                                \
                                                            GNU_MemOrder);)                              \
      BSON_IF_GNU_LEGACY_ATOMICS (__typeof__ (ExpectActualVar) _val;                                     \
                                  _val = __sync_val_compare_and_swap (Ptr, ExpectActualVar, NewValue);   \
                                  ExpectActualVar = _val;)                                               \
   } while (0)


#define DECL_ATOMIC_INTEGRAL(NamePart, Type, VCIntrinSuffix)                                                           \
   static BSON_INLINE Type bson_atomic_##NamePart##_fetch_add (                                                        \
      Type volatile *a, Type addend, enum bson_memory_order ord)                                                       \
   {                                                                                                                   \
      DEF_ATOMIC_OP (BSON_CONCAT (_InterlockedExchangeAdd, VCIntrinSuffix),                                            \
                     __atomic_fetch_add,                                                                               \
                     __sync_fetch_and_add,                                                                             \
                     ord,                                                                                              \
                     a,                                                                                                \
                     addend);                                                                                          \
   }                                                                                                                   \
                                                                                                                       \
   static BSON_INLINE Type bson_atomic_##NamePart##_fetch_sub (                                                        \
      Type volatile *a, Type subtrahend, enum bson_memory_order ord)                                                   \
   {                                                                                                                   \
      /* MSVC doesn't have a subtract intrinsic, so just reuse addition    */                                          \
      BSON_IF_MSVC (return bson_atomic_##NamePart##_fetch_add (a, -subtrahend, ord);)                                  \
      BSON_IF_GNU_LIKE (DEF_ATOMIC_OP (~, __atomic_fetch_sub, ~, ord, a, subtrahend);)                                 \
      BSON_IF_GNU_LEGACY_ATOMICS (DEF_ATOMIC_OP (~, ~, __sync_fetch_and_sub, ord, a, subtrahend);)                     \
   }                                                                                                                   \
                                                                                                                       \
   static BSON_INLINE Type bson_atomic_##NamePart##_fetch (Type volatile const *a, enum bson_memory_order order)       \
   {                                                                                                                   \
      /* MSVC doesn't have a load intrinsic, so just add zero */                                                       \
      BSON_IF_MSVC (return bson_atomic_##NamePart##_fetch_add ((Type volatile *) a, 0, order);)                        \
      /* GNU doesn't want RELEASE order for the fetch operation, so we can't                                           \
       * just use DEF_ATOMIC_OP. */                                                                                    \
      BSON_IF_GNU_LIKE (switch (order) {                                                                               \
         case bson_memory_order_release: /* Fall back to seqcst */                                                     \
         case bson_memory_order_acq_rel: /* Fall back to seqcst */                                                     \
         case bson_memory_order_seq_cst:                                                                               \
            return __atomic_load_n (a, __ATOMIC_SEQ_CST);                                                              \
         case bson_memory_order_acquire:                                                                               \
            return __atomic_load_n (a, __ATOMIC_ACQUIRE);                                                              \
         case bson_memory_order_consume:                                                                               \
            return __atomic_load_n (a, __ATOMIC_CONSUME);                                                              \
         case bson_memory_order_relaxed:                                                                               \
            return __atomic_load_n (a, __ATOMIC_RELAXED);                                                              \
         default:                                                                                                      \
            BSON_UNREACHABLE ("Invalid bson_memory_order value");                                                      \
      })                                                                                                               \
      BSON_IF_GNU_LEGACY_ATOMICS ({                                                                                    \
         __sync_synchronize ();                                                                                        \
         return *a;                                                                                                    \
      })                                                                                                               \
   }                                                                                                                   \
                                                                                                                       \
   static BSON_INLINE Type bson_atomic_##NamePart##_exchange (                                                         \
      Type volatile *a, Type value, enum bson_memory_order ord)                                                        \
   {                                                                                                                   \
      BSON_IF_MSVC (DEF_ATOMIC_OP (BSON_CONCAT (_InterlockedExchange, VCIntrinSuffix), ~, ~, ord, a, value);)          \
      /* GNU doesn't want CONSUME order for the exchange operation, so we                                              \
       * cannot use DEF_ATOMIC_OP. */                                                                                  \
      BSON_IF_GNU_LIKE (switch (ord) {                                                                                 \
         case bson_memory_order_acq_rel:                                                                               \
            return __atomic_exchange_n (a, value, __ATOMIC_ACQ_REL);                                                   \
         case bson_memory_order_release:                                                                               \
            return __atomic_exchange_n (a, value, __ATOMIC_RELEASE);                                                   \
         case bson_memory_order_seq_cst:                                                                               \
            return __atomic_exchange_n (a, value, __ATOMIC_SEQ_CST);                                                   \
         case bson_memory_order_consume: /* Fall back to acquire */                                                    \
         case bson_memory_order_acquire:                                                                               \
            return __atomic_exchange_n (a, value, __ATOMIC_ACQUIRE);                                                   \
         case bson_memory_order_relaxed:                                                                               \
            return __atomic_exchange_n (a, value, __ATOMIC_RELAXED);                                                   \
         default:                                                                                                      \
            BSON_UNREACHABLE ("Invalid bson_memory_order value");                                                      \
      })                                                                                                               \
      BSON_IF_GNU_LEGACY_ATOMICS (return __sync_val_compare_and_swap (a, *a, value);)                                  \
   }                                                                                                                   \
                                                                                                                       \
   static BSON_INLINE Type bson_atomic_##NamePart##_compare_exchange_strong (                                          \
      Type volatile *a, Type expect, Type new_value, enum bson_memory_order ord)                                       \
   {                                                                                                                   \
      Type actual = expect;                                                                                            \
      switch (ord) {                                                                                                   \
      case bson_memory_order_release:                                                                                  \
      case bson_memory_order_acq_rel:                                                                                  \
      case bson_memory_order_seq_cst:                                                                                  \
         DEF_ATOMIC_CMPEXCH_STRONG (VCIntrinSuffix, , __ATOMIC_SEQ_CST, a, actual, new_value);                         \
         break;                                                                                                        \
      case bson_memory_order_acquire:                                                                                  \
         DEF_ATOMIC_CMPEXCH_STRONG (                                                                                   \
            VCIntrinSuffix, MSVC_MEMORDER_SUFFIX (_acq), __ATOMIC_ACQUIRE, a, actual, new_value);                      \
         break;                                                                                                        \
      case bson_memory_order_consume:                                                                                  \
         DEF_ATOMIC_CMPEXCH_STRONG (                                                                                   \
            VCIntrinSuffix, MSVC_MEMORDER_SUFFIX (_acq), __ATOMIC_CONSUME, a, actual, new_value);                      \
         break;                                                                                                        \
      case bson_memory_order_relaxed:                                                                                  \
         DEF_ATOMIC_CMPEXCH_STRONG (                                                                                   \
            VCIntrinSuffix, MSVC_MEMORDER_SUFFIX (_nf), __ATOMIC_RELAXED, a, actual, new_value);                       \
         break;                                                                                                        \
      default:                                                                                                         \
         BSON_UNREACHABLE ("Invalid bson_memory_order value");                                                         \
      }                                                                                                                \
      return actual;                                                                                                   \
   }                                                                                                                   \
                                                                                                                       \
   static BSON_INLINE Type bson_atomic_##NamePart##_compare_exchange_weak (                                            \
      Type volatile *a, Type expect, Type new_value, enum bson_memory_order ord)                                       \
   {                                                                                                                   \
      Type actual = expect;                                                                                            \
      switch (ord) {                                                                                                   \
      case bson_memory_order_release:                                                                                  \
      case bson_memory_order_acq_rel:                                                                                  \
      case bson_memory_order_seq_cst:                                                                                  \
         DEF_ATOMIC_CMPEXCH_WEAK (VCIntrinSuffix, , __ATOMIC_SEQ_CST, a, actual, new_value);                           \
         break;                                                                                                        \
      case bson_memory_order_acquire:                                                                                  \
         DEF_ATOMIC_CMPEXCH_WEAK (                                                                                     \
            VCIntrinSuffix, MSVC_MEMORDER_SUFFIX (_acq), __ATOMIC_ACQUIRE, a, actual, new_value);                      \
         break;                                                                                                        \
      case bson_memory_order_consume:                                                                                  \
         DEF_ATOMIC_CMPEXCH_WEAK (                                                                                     \
            VCIntrinSuffix, MSVC_MEMORDER_SUFFIX (_acq), __ATOMIC_CONSUME, a, actual, new_value);                      \
         break;                                                                                                        \
      case bson_memory_order_relaxed:                                                                                  \
         DEF_ATOMIC_CMPEXCH_WEAK (VCIntrinSuffix, MSVC_MEMORDER_SUFFIX (_nf), __ATOMIC_RELAXED, a, actual, new_value); \
         break;                                                                                                        \
      default:                                                                                                         \
         BSON_UNREACHABLE ("Invalid bson_memory_order value");                                                         \
      }                                                                                                                \
      return actual;                                                                                                   \
   }

#define DECL_ATOMIC_STDINT(Name, VCSuffix) DECL_ATOMIC_INTEGRAL (Name, Name##_t, VCSuffix)

#if defined(_MSC_VER) || defined(BSON_USE_LEGACY_GCC_ATOMICS)
/* MSVC and GCC require built-in types (not typedefs) for their atomic
 * intrinsics. */
#if defined(_MSC_VER)
#define DECL_ATOMIC_INTEGRAL_INT8 char
#define DECL_ATOMIC_INTEGRAL_INT32 long
#define DECL_ATOMIC_INTEGRAL_INT long
#else
#define DECL_ATOMIC_INTEGRAL_INT8 signed char
#define DECL_ATOMIC_INTEGRAL_INT32 int
#define DECL_ATOMIC_INTEGRAL_INT int
#endif
DECL_ATOMIC_INTEGRAL (int8, DECL_ATOMIC_INTEGRAL_INT8, 8)
DECL_ATOMIC_INTEGRAL (int16, short, 16)
#if !defined(BSON_EMULATE_INT32)
DECL_ATOMIC_INTEGRAL (int32, DECL_ATOMIC_INTEGRAL_INT32, )
#endif
#if !defined(BSON_EMULATE_INT)
DECL_ATOMIC_INTEGRAL (int, DECL_ATOMIC_INTEGRAL_INT, )
#endif
#else
/* Other compilers that we support provide generic intrinsics */
DECL_ATOMIC_STDINT (int8, 8)
DECL_ATOMIC_STDINT (int16, 16)
#if !defined(BSON_EMULATE_INT32)
DECL_ATOMIC_STDINT (int32, )
#endif
#if !defined(BSON_EMULATE_INT)
DECL_ATOMIC_INTEGRAL (int, int, )
#endif
#endif

#ifndef DECL_ATOMIC_INTEGRAL_INT32
#define DECL_ATOMIC_INTEGRAL_INT32 int32_t
#endif

BSON_EXPORT (int64_t)
_bson_emul_atomic_int64_fetch_add (int64_t volatile *val, int64_t v, enum bson_memory_order);
BSON_EXPORT (int64_t)
_bson_emul_atomic_int64_exchange (int64_t volatile *val, int64_t v, enum bson_memory_order);
BSON_EXPORT (int64_t)
_bson_emul_atomic_int64_compare_exchange_strong (int64_t volatile *val,
                                                 int64_t expect_value,
                                                 int64_t new_value,
                                                 enum bson_memory_order);

BSON_EXPORT (int64_t)
_bson_emul_atomic_int64_compare_exchange_weak (int64_t volatile *val,
                                               int64_t expect_value,
                                               int64_t new_value,
                                               enum bson_memory_order);

BSON_EXPORT (int32_t)
_bson_emul_atomic_int32_fetch_add (int32_t volatile *val, int32_t v, enum bson_memory_order);
BSON_EXPORT (int32_t)
_bson_emul_atomic_int32_exchange (int32_t volatile *val, int32_t v, enum bson_memory_order);
BSON_EXPORT (int32_t)
_bson_emul_atomic_int32_compare_exchange_strong (int32_t volatile *val,
                                                 int32_t expect_value,
                                                 int32_t new_value,
                                                 enum bson_memory_order);

BSON_EXPORT (int32_t)
_bson_emul_atomic_int32_compare_exchange_weak (int32_t volatile *val,
                                               int32_t expect_value,
                                               int32_t new_value,
                                               enum bson_memory_order);

BSON_EXPORT (int)
_bson_emul_atomic_int_fetch_add (int volatile *val, int v, enum bson_memory_order);
BSON_EXPORT (int)
_bson_emul_atomic_int_exchange (int volatile *val, int v, enum bson_memory_order);
BSON_EXPORT (int)
_bson_emul_atomic_int_compare_exchange_strong (int volatile *val,
                                               int expect_value,
                                               int new_value,
                                               enum bson_memory_order);

BSON_EXPORT (int)
_bson_emul_atomic_int_compare_exchange_weak (int volatile *val,
                                             int expect_value,
                                             int new_value,
                                             enum bson_memory_order);

BSON_EXPORT (void *)
_bson_emul_atomic_ptr_exchange (void *volatile *val, void *v, enum bson_memory_order);

BSON_EXPORT (void)
bson_thrd_yield (void);

#if (defined(_MSC_VER) && !defined(_M_IX86)) || (defined(__LP64__) && __LP64__)
/* (64-bit intrinsics are only available in x64) */
#ifdef _MSC_VER
DECL_ATOMIC_INTEGRAL (int64, __int64, 64)
#else
DECL_ATOMIC_STDINT (int64, 64)
#endif
#else
static BSON_INLINE int64_t
bson_atomic_int64_fetch (const int64_t volatile *val, enum bson_memory_order order)
{
   return _bson_emul_atomic_int64_fetch_add ((int64_t volatile *) val, 0, order);
}

static BSON_INLINE int64_t
bson_atomic_int64_fetch_add (int64_t volatile *val, int64_t v, enum bson_memory_order order)
{
   return _bson_emul_atomic_int64_fetch_add (val, v, order);
}

static BSON_INLINE int64_t
bson_atomic_int64_fetch_sub (int64_t volatile *val, int64_t v, enum bson_memory_order order)
{
   return _bson_emul_atomic_int64_fetch_add (val, -v, order);
}

static BSON_INLINE int64_t
bson_atomic_int64_exchange (int64_t volatile *val, int64_t v, enum bson_memory_order order)
{
   return _bson_emul_atomic_int64_exchange (val, v, order);
}

static BSON_INLINE int64_t
bson_atomic_int64_compare_exchange_strong (int64_t volatile *val,
                                           int64_t expect_value,
                                           int64_t new_value,
                                           enum bson_memory_order order)
{
   return _bson_emul_atomic_int64_compare_exchange_strong (val, expect_value, new_value, order);
}

static BSON_INLINE int64_t
bson_atomic_int64_compare_exchange_weak (int64_t volatile *val,
                                         int64_t expect_value,
                                         int64_t new_value,
                                         enum bson_memory_order order)
{
   return _bson_emul_atomic_int64_compare_exchange_weak (val, expect_value, new_value, order);
}
#endif

#if defined(BSON_EMULATE_INT32)
static BSON_INLINE int32_t
bson_atomic_int32_fetch (const int32_t volatile *val, enum bson_memory_order order)
{
   return _bson_emul_atomic_int32_fetch_add ((int32_t volatile *) val, 0, order);
}

static BSON_INLINE int32_t
bson_atomic_int32_fetch_add (int32_t volatile *val, int32_t v, enum bson_memory_order order)
{
   return _bson_emul_atomic_int32_fetch_add (val, v, order);
}

static BSON_INLINE int32_t
bson_atomic_int32_fetch_sub (int32_t volatile *val, int32_t v, enum bson_memory_order order)
{
   return _bson_emul_atomic_int32_fetch_add (val, -v, order);
}

static BSON_INLINE int32_t
bson_atomic_int32_exchange (int32_t volatile *val, int32_t v, enum bson_memory_order order)
{
   return _bson_emul_atomic_int32_exchange (val, v, order);
}

static BSON_INLINE int32_t
bson_atomic_int32_compare_exchange_strong (int32_t volatile *val,
                                           int32_t expect_value,
                                           int32_t new_value,
                                           enum bson_memory_order order)
{
   return _bson_emul_atomic_int32_compare_exchange_strong (val, expect_value, new_value, order);
}

static BSON_INLINE int32_t
bson_atomic_int32_compare_exchange_weak (int32_t volatile *val,
                                         int32_t expect_value,
                                         int32_t new_value,
                                         enum bson_memory_order order)
{
   return _bson_emul_atomic_int32_compare_exchange_weak (val, expect_value, new_value, order);
}
#endif /* BSON_EMULATE_INT32 */

#if defined(BSON_EMULATE_INT)
static BSON_INLINE int
bson_atomic_int_fetch (const int volatile *val, enum bson_memory_order order)
{
   return _bson_emul_atomic_int_fetch_add ((int volatile *) val, 0, order);
}

static BSON_INLINE int
bson_atomic_int_fetch_add (int volatile *val, int v, enum bson_memory_order order)
{
   return _bson_emul_atomic_int_fetch_add (val, v, order);
}

static BSON_INLINE int
bson_atomic_int_fetch_sub (int volatile *val, int v, enum bson_memory_order order)
{
   return _bson_emul_atomic_int_fetch_add (val, -v, order);
}

static BSON_INLINE int
bson_atomic_int_exchange (int volatile *val, int v, enum bson_memory_order order)
{
   return _bson_emul_atomic_int_exchange (val, v, order);
}

static BSON_INLINE int
bson_atomic_int_compare_exchange_strong (int volatile *val,
                                         int expect_value,
                                         int new_value,
                                         enum bson_memory_order order)
{
   return _bson_emul_atomic_int_compare_exchange_strong (val, expect_value, new_value, order);
}

static BSON_INLINE int
bson_atomic_int_compare_exchange_weak (int volatile *val, int expect_value, int new_value, enum bson_memory_order order)
{
   return _bson_emul_atomic_int_compare_exchange_weak (val, expect_value, new_value, order);
}
#endif /* BSON_EMULATE_INT */

static BSON_INLINE void *
bson_atomic_ptr_exchange (void *volatile *ptr, void *new_value, enum bson_memory_order ord)
{
#if defined(BSON_EMULATE_PTR)
   return _bson_emul_atomic_ptr_exchange (ptr, new_value, ord);
#elif defined(BSON_USE_LEGACY_GCC_ATOMICS)
   /* The older __sync_val_compare_and_swap also takes oldval */
   DEF_ATOMIC_OP (_InterlockedExchangePointer, , __sync_val_compare_and_swap, ord, ptr, *ptr, new_value);
#else
   DEF_ATOMIC_OP (_InterlockedExchangePointer, __atomic_exchange_n, , ord, ptr, new_value);
#endif
}

static BSON_INLINE void *
bson_atomic_ptr_compare_exchange_strong (void *volatile *ptr, void *expect, void *new_value, enum bson_memory_order ord)
{
   switch (ord) {
   case bson_memory_order_release:
   case bson_memory_order_acq_rel:
   case bson_memory_order_seq_cst:
      DEF_ATOMIC_CMPEXCH_STRONG (Pointer, , __ATOMIC_SEQ_CST, ptr, expect, new_value);
      return expect;
   case bson_memory_order_relaxed:
      DEF_ATOMIC_CMPEXCH_STRONG (Pointer, MSVC_MEMORDER_SUFFIX (_nf), __ATOMIC_RELAXED, ptr, expect, new_value);
      return expect;
   case bson_memory_order_consume:
      DEF_ATOMIC_CMPEXCH_STRONG (Pointer, MSVC_MEMORDER_SUFFIX (_acq), __ATOMIC_CONSUME, ptr, expect, new_value);
      return expect;
   case bson_memory_order_acquire:
      DEF_ATOMIC_CMPEXCH_STRONG (Pointer, MSVC_MEMORDER_SUFFIX (_acq), __ATOMIC_ACQUIRE, ptr, expect, new_value);
      return expect;
   default:
      BSON_UNREACHABLE ("Invalid bson_memory_order value");
   }
}


static BSON_INLINE void *
bson_atomic_ptr_compare_exchange_weak (void *volatile *ptr, void *expect, void *new_value, enum bson_memory_order ord)
{
   switch (ord) {
   case bson_memory_order_release:
   case bson_memory_order_acq_rel:
   case bson_memory_order_seq_cst:
      DEF_ATOMIC_CMPEXCH_WEAK (Pointer, , __ATOMIC_SEQ_CST, ptr, expect, new_value);
      return expect;
   case bson_memory_order_relaxed:
      DEF_ATOMIC_CMPEXCH_WEAK (Pointer, MSVC_MEMORDER_SUFFIX (_nf), __ATOMIC_RELAXED, ptr, expect, new_value);
      return expect;
   case bson_memory_order_consume:
      DEF_ATOMIC_CMPEXCH_WEAK (Pointer, MSVC_MEMORDER_SUFFIX (_acq), __ATOMIC_CONSUME, ptr, expect, new_value);
      return expect;
   case bson_memory_order_acquire:
      DEF_ATOMIC_CMPEXCH_WEAK (Pointer, MSVC_MEMORDER_SUFFIX (_acq), __ATOMIC_ACQUIRE, ptr, expect, new_value);
      return expect;
   default:
      BSON_UNREACHABLE ("Invalid bson_memory_order value");
   }
}


static BSON_INLINE void *
bson_atomic_ptr_fetch (void *volatile const *ptr, enum bson_memory_order ord)
{
   return bson_atomic_ptr_compare_exchange_strong ((void *volatile *) ptr, NULL, NULL, ord);
}

#undef DECL_ATOMIC_STDINT
#undef DECL_ATOMIC_INTEGRAL
#undef DEF_ATOMIC_OP
#undef DEF_ATOMIC_CMPEXCH_STRONG
#undef DEF_ATOMIC_CMPEXCH_WEAK
#undef MSVC_MEMORDER_SUFFIX

/**
 * @brief Generate a full-fence memory barrier at the call site.
 */
static BSON_INLINE void
bson_atomic_thread_fence (void)
{
   BSON_IF_MSVC (MemoryBarrier ();)
   BSON_IF_GNU_LIKE (__sync_synchronize ();)
   BSON_IF_GNU_LEGACY_ATOMICS (__sync_synchronize ();)
}

#ifdef BSON_USE_LEGACY_GCC_ATOMICS
#undef BSON_IF_GNU_LIKE
#define BSON_IF_GNU_LIKE(...) __VA_ARGS__
#endif
#undef BSON_IF_GNU_LEGACY_ATOMICS
#undef BSON_USE_LEGACY_GCC_ATOMICS

BSON_GNUC_DEPRECATED_FOR ("bson_atomic_thread_fence")
BSON_EXPORT (void) bson_memory_barrier (void);

BSON_GNUC_DEPRECATED_FOR ("bson_atomic_int_fetch_add")
BSON_EXPORT (int32_t) bson_atomic_int_add (volatile int32_t *p, int32_t n);

BSON_GNUC_DEPRECATED_FOR ("bson_atomic_int64_fetch_add")
BSON_EXPORT (int64_t) bson_atomic_int64_add (volatile int64_t *p, int64_t n);


#undef BSON_EMULATE_PTR
#undef BSON_EMULATE_INT32
#undef BSON_EMULATE_INT

BSON_END_DECLS


#endif /* BSON_ATOMIC_H */
