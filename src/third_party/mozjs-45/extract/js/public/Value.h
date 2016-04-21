/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* JS::Value implementation. */

#ifndef js_Value_h
#define js_Value_h

#include "mozilla/Attributes.h"
#include "mozilla/FloatingPoint.h"
#include "mozilla/Likely.h"

#include <limits> /* for std::numeric_limits */

#include "js-config.h"
#include "jstypes.h"

#include "js/GCAPI.h"
#include "js/RootingAPI.h"
#include "js/Utility.h"

namespace JS { class Value; }

/* JS::Value can store a full int32_t. */
#define JSVAL_INT_BITS          32
#define JSVAL_INT_MIN           ((int32_t)0x80000000)
#define JSVAL_INT_MAX           ((int32_t)0x7fffffff)

/*
 * Try to get jsvals 64-bit aligned. We could almost assert that all values are
 * aligned, but MSVC and GCC occasionally break alignment.
 */
#if defined(__GNUC__) || defined(__xlc__) || defined(__xlC__)
# define JSVAL_ALIGNMENT        __attribute__((aligned (8)))
#elif defined(_MSC_VER)
  /*
   * Structs can be aligned with MSVC, but not if they are used as parameters,
   * so we just don't try to align.
   */
# define JSVAL_ALIGNMENT
#elif defined(__SUNPRO_C) || defined(__SUNPRO_CC)
# define JSVAL_ALIGNMENT
#elif defined(__HP_cc) || defined(__HP_aCC)
# define JSVAL_ALIGNMENT
#endif

#if defined(JS_PUNBOX64)
# define JSVAL_TAG_SHIFT 47
#endif

/*
 * We try to use enums so that printing a jsval_layout in the debugger shows
 * nice symbolic type tags, however we can only do this when we can force the
 * underlying type of the enum to be the desired size.
 */
#if !defined(__SUNPRO_CC) && !defined(__xlC__)

#if defined(_MSC_VER)
# define JS_ENUM_HEADER(id, type)              enum id : type
# define JS_ENUM_FOOTER(id)
#else
# define JS_ENUM_HEADER(id, type)              enum id
# define JS_ENUM_FOOTER(id)                    __attribute__((packed))
#endif

/* Remember to propagate changes to the C defines below. */
JS_ENUM_HEADER(JSValueType, uint8_t)
{
    JSVAL_TYPE_DOUBLE              = 0x00,
    JSVAL_TYPE_INT32               = 0x01,
    JSVAL_TYPE_UNDEFINED           = 0x02,
    JSVAL_TYPE_BOOLEAN             = 0x03,
    JSVAL_TYPE_MAGIC               = 0x04,
    JSVAL_TYPE_STRING              = 0x05,
    JSVAL_TYPE_SYMBOL              = 0x06,
    JSVAL_TYPE_NULL                = 0x07,
    JSVAL_TYPE_OBJECT              = 0x08,

    /* These never appear in a jsval; they are only provided as an out-of-band value. */
    JSVAL_TYPE_UNKNOWN             = 0x20,
    JSVAL_TYPE_MISSING             = 0x21
} JS_ENUM_FOOTER(JSValueType);

static_assert(sizeof(JSValueType) == 1,
              "compiler typed enum support is apparently buggy");

#if defined(JS_NUNBOX32)

/* Remember to propagate changes to the C defines below. */
JS_ENUM_HEADER(JSValueTag, uint32_t)
{
    JSVAL_TAG_CLEAR                = 0xFFFFFF80,
    JSVAL_TAG_INT32                = JSVAL_TAG_CLEAR | JSVAL_TYPE_INT32,
    JSVAL_TAG_UNDEFINED            = JSVAL_TAG_CLEAR | JSVAL_TYPE_UNDEFINED,
    JSVAL_TAG_STRING               = JSVAL_TAG_CLEAR | JSVAL_TYPE_STRING,
    JSVAL_TAG_SYMBOL               = JSVAL_TAG_CLEAR | JSVAL_TYPE_SYMBOL,
    JSVAL_TAG_BOOLEAN              = JSVAL_TAG_CLEAR | JSVAL_TYPE_BOOLEAN,
    JSVAL_TAG_MAGIC                = JSVAL_TAG_CLEAR | JSVAL_TYPE_MAGIC,
    JSVAL_TAG_NULL                 = JSVAL_TAG_CLEAR | JSVAL_TYPE_NULL,
    JSVAL_TAG_OBJECT               = JSVAL_TAG_CLEAR | JSVAL_TYPE_OBJECT
} JS_ENUM_FOOTER(JSValueTag);

static_assert(sizeof(JSValueTag) == sizeof(uint32_t),
              "compiler typed enum support is apparently buggy");

#elif defined(JS_PUNBOX64)

/* Remember to propagate changes to the C defines below. */
JS_ENUM_HEADER(JSValueTag, uint32_t)
{
    JSVAL_TAG_MAX_DOUBLE           = 0x1FFF0,
    JSVAL_TAG_INT32                = JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_INT32,
    JSVAL_TAG_UNDEFINED            = JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_UNDEFINED,
    JSVAL_TAG_STRING               = JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_STRING,
    JSVAL_TAG_SYMBOL               = JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_SYMBOL,
    JSVAL_TAG_BOOLEAN              = JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_BOOLEAN,
    JSVAL_TAG_MAGIC                = JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_MAGIC,
    JSVAL_TAG_NULL                 = JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_NULL,
    JSVAL_TAG_OBJECT               = JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_OBJECT
} JS_ENUM_FOOTER(JSValueTag);

static_assert(sizeof(JSValueTag) == sizeof(uint32_t),
              "compiler typed enum support is apparently buggy");

JS_ENUM_HEADER(JSValueShiftedTag, uint64_t)
{
    JSVAL_SHIFTED_TAG_MAX_DOUBLE   = ((((uint64_t)JSVAL_TAG_MAX_DOUBLE) << JSVAL_TAG_SHIFT) | 0xFFFFFFFF),
    JSVAL_SHIFTED_TAG_INT32        = (((uint64_t)JSVAL_TAG_INT32)      << JSVAL_TAG_SHIFT),
    JSVAL_SHIFTED_TAG_UNDEFINED    = (((uint64_t)JSVAL_TAG_UNDEFINED)  << JSVAL_TAG_SHIFT),
    JSVAL_SHIFTED_TAG_STRING       = (((uint64_t)JSVAL_TAG_STRING)     << JSVAL_TAG_SHIFT),
    JSVAL_SHIFTED_TAG_SYMBOL       = (((uint64_t)JSVAL_TAG_SYMBOL)     << JSVAL_TAG_SHIFT),
    JSVAL_SHIFTED_TAG_BOOLEAN      = (((uint64_t)JSVAL_TAG_BOOLEAN)    << JSVAL_TAG_SHIFT),
    JSVAL_SHIFTED_TAG_MAGIC        = (((uint64_t)JSVAL_TAG_MAGIC)      << JSVAL_TAG_SHIFT),
    JSVAL_SHIFTED_TAG_NULL         = (((uint64_t)JSVAL_TAG_NULL)       << JSVAL_TAG_SHIFT),
    JSVAL_SHIFTED_TAG_OBJECT       = (((uint64_t)JSVAL_TAG_OBJECT)     << JSVAL_TAG_SHIFT)
} JS_ENUM_FOOTER(JSValueShiftedTag);

static_assert(sizeof(JSValueShiftedTag) == sizeof(uint64_t),
              "compiler typed enum support is apparently buggy");

#endif

/*
 * All our supported compilers implement C++11 |enum Foo : T| syntax, so don't
 * expose these macros. (This macro exists *only* because gcc bug 51242
 * <https://gcc.gnu.org/bugzilla/show_bug.cgi?id=51242> makes bit-fields of
 * typed enums trigger a warning that can't be turned off. Don't expose it
 * beyond this file!)
 */
#undef JS_ENUM_HEADER
#undef JS_ENUM_FOOTER

#else  /* !defined(__SUNPRO_CC) && !defined(__xlC__) */

typedef uint8_t JSValueType;
#define JSVAL_TYPE_DOUBLE            ((uint8_t)0x00)
#define JSVAL_TYPE_INT32             ((uint8_t)0x01)
#define JSVAL_TYPE_UNDEFINED         ((uint8_t)0x02)
#define JSVAL_TYPE_BOOLEAN           ((uint8_t)0x03)
#define JSVAL_TYPE_MAGIC             ((uint8_t)0x04)
#define JSVAL_TYPE_STRING            ((uint8_t)0x05)
#define JSVAL_TYPE_SYMBOL            ((uint8_t)0x06)
#define JSVAL_TYPE_NULL              ((uint8_t)0x07)
#define JSVAL_TYPE_OBJECT            ((uint8_t)0x08)
#define JSVAL_TYPE_UNKNOWN           ((uint8_t)0x20)

#if defined(JS_NUNBOX32)

typedef uint32_t JSValueTag;
#define JSVAL_TAG_CLEAR              ((uint32_t)(0xFFFFFF80))
#define JSVAL_TAG_INT32              ((uint32_t)(JSVAL_TAG_CLEAR | JSVAL_TYPE_INT32))
#define JSVAL_TAG_UNDEFINED          ((uint32_t)(JSVAL_TAG_CLEAR | JSVAL_TYPE_UNDEFINED))
#define JSVAL_TAG_STRING             ((uint32_t)(JSVAL_TAG_CLEAR | JSVAL_TYPE_STRING))
#define JSVAL_TAG_SYMBOL             ((uint32_t)(JSVAL_TAG_CLEAR | JSVAL_TYPE_SYMBOL))
#define JSVAL_TAG_BOOLEAN            ((uint32_t)(JSVAL_TAG_CLEAR | JSVAL_TYPE_BOOLEAN))
#define JSVAL_TAG_MAGIC              ((uint32_t)(JSVAL_TAG_CLEAR | JSVAL_TYPE_MAGIC))
#define JSVAL_TAG_NULL               ((uint32_t)(JSVAL_TAG_CLEAR | JSVAL_TYPE_NULL))
#define JSVAL_TAG_OBJECT             ((uint32_t)(JSVAL_TAG_CLEAR | JSVAL_TYPE_OBJECT))

#elif defined(JS_PUNBOX64)

typedef uint32_t JSValueTag;
#define JSVAL_TAG_MAX_DOUBLE         ((uint32_t)(0x1FFF0))
#define JSVAL_TAG_INT32              (uint32_t)(JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_INT32)
#define JSVAL_TAG_UNDEFINED          (uint32_t)(JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_UNDEFINED)
#define JSVAL_TAG_STRING             (uint32_t)(JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_STRING)
#define JSVAL_TAG_SYMBOL             (uint32_t)(JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_SYMBOL)
#define JSVAL_TAG_BOOLEAN            (uint32_t)(JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_BOOLEAN)
#define JSVAL_TAG_MAGIC              (uint32_t)(JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_MAGIC)
#define JSVAL_TAG_NULL               (uint32_t)(JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_NULL)
#define JSVAL_TAG_OBJECT             (uint32_t)(JSVAL_TAG_MAX_DOUBLE | JSVAL_TYPE_OBJECT)

typedef uint64_t JSValueShiftedTag;
#define JSVAL_SHIFTED_TAG_MAX_DOUBLE ((((uint64_t)JSVAL_TAG_MAX_DOUBLE) << JSVAL_TAG_SHIFT) | 0xFFFFFFFF)
#define JSVAL_SHIFTED_TAG_INT32      (((uint64_t)JSVAL_TAG_INT32)      << JSVAL_TAG_SHIFT)
#define JSVAL_SHIFTED_TAG_UNDEFINED  (((uint64_t)JSVAL_TAG_UNDEFINED)  << JSVAL_TAG_SHIFT)
#define JSVAL_SHIFTED_TAG_STRING     (((uint64_t)JSVAL_TAG_STRING)     << JSVAL_TAG_SHIFT)
#define JSVAL_SHIFTED_TAG_SYMBOL     (((uint64_t)JSVAL_TAG_SYMBOL)     << JSVAL_TAG_SHIFT)
#define JSVAL_SHIFTED_TAG_BOOLEAN    (((uint64_t)JSVAL_TAG_BOOLEAN)    << JSVAL_TAG_SHIFT)
#define JSVAL_SHIFTED_TAG_MAGIC      (((uint64_t)JSVAL_TAG_MAGIC)      << JSVAL_TAG_SHIFT)
#define JSVAL_SHIFTED_TAG_NULL       (((uint64_t)JSVAL_TAG_NULL)       << JSVAL_TAG_SHIFT)
#define JSVAL_SHIFTED_TAG_OBJECT     (((uint64_t)JSVAL_TAG_OBJECT)     << JSVAL_TAG_SHIFT)

#endif  /* JS_PUNBOX64 */
#endif  /* !defined(__SUNPRO_CC) && !defined(__xlC__) */

#if defined(JS_NUNBOX32)

#define JSVAL_TYPE_TO_TAG(type)      ((JSValueTag)(JSVAL_TAG_CLEAR | (type)))

#define JSVAL_LOWER_INCL_TAG_OF_OBJ_OR_NULL_SET         JSVAL_TAG_NULL
#define JSVAL_UPPER_EXCL_TAG_OF_PRIMITIVE_SET           JSVAL_TAG_OBJECT
#define JSVAL_UPPER_INCL_TAG_OF_NUMBER_SET              JSVAL_TAG_INT32
#define JSVAL_LOWER_INCL_TAG_OF_GCTHING_SET             JSVAL_TAG_STRING

#elif defined(JS_PUNBOX64)

#define JSVAL_PAYLOAD_MASK           0x00007FFFFFFFFFFFLL
#define JSVAL_TAG_MASK               0xFFFF800000000000LL
#define JSVAL_TYPE_TO_TAG(type)      ((JSValueTag)(JSVAL_TAG_MAX_DOUBLE | (type)))
#define JSVAL_TYPE_TO_SHIFTED_TAG(type) (((uint64_t)JSVAL_TYPE_TO_TAG(type)) << JSVAL_TAG_SHIFT)

#define JSVAL_LOWER_INCL_TAG_OF_OBJ_OR_NULL_SET         JSVAL_TAG_NULL
#define JSVAL_UPPER_EXCL_TAG_OF_PRIMITIVE_SET           JSVAL_TAG_OBJECT
#define JSVAL_UPPER_INCL_TAG_OF_NUMBER_SET              JSVAL_TAG_INT32
#define JSVAL_LOWER_INCL_TAG_OF_GCTHING_SET             JSVAL_TAG_STRING

#define JSVAL_LOWER_INCL_SHIFTED_TAG_OF_OBJ_OR_NULL_SET  JSVAL_SHIFTED_TAG_NULL
#define JSVAL_UPPER_EXCL_SHIFTED_TAG_OF_PRIMITIVE_SET    JSVAL_SHIFTED_TAG_OBJECT
#define JSVAL_UPPER_EXCL_SHIFTED_TAG_OF_NUMBER_SET       JSVAL_SHIFTED_TAG_UNDEFINED
#define JSVAL_LOWER_INCL_SHIFTED_TAG_OF_GCTHING_SET      JSVAL_SHIFTED_TAG_STRING

#endif /* JS_PUNBOX64 */

typedef enum JSWhyMagic
{
    /** a hole in a native object's elements */
    JS_ELEMENTS_HOLE,

    /** there is not a pending iterator value */
    JS_NO_ITER_VALUE,

    /** exception value thrown when closing a generator */
    JS_GENERATOR_CLOSING,

    /** compiler sentinel value */
    JS_NO_CONSTANT,

    /** used in debug builds to catch tracing errors */
    JS_THIS_POISON,

    /** used in debug builds to catch tracing errors */
    JS_ARG_POISON,

    /** an empty subnode in the AST serializer */
    JS_SERIALIZE_NO_NODE,

    /** lazy arguments value on the stack */
    JS_LAZY_ARGUMENTS,

    /** optimized-away 'arguments' value */
    JS_OPTIMIZED_ARGUMENTS,

    /** magic value passed to natives to indicate construction */
    JS_IS_CONSTRUCTING,

    /** arguments.callee has been overwritten */
    JS_OVERWRITTEN_CALLEE,

    /** value of static block object slot */
    JS_BLOCK_NEEDS_CLONE,

    /** see class js::HashableValue */
    JS_HASH_KEY_EMPTY,

    /** error while running Ion code */
    JS_ION_ERROR,

    /** missing recover instruction result */
    JS_ION_BAILOUT,

    /** optimized out slot */
    JS_OPTIMIZED_OUT,

    /** uninitialized lexical bindings that produce ReferenceError on touch. */
    JS_UNINITIALIZED_LEXICAL,

    /** for local use */
    JS_GENERIC_MAGIC,

    JS_WHY_MAGIC_COUNT
} JSWhyMagic;

#if defined(IS_LITTLE_ENDIAN)
# if defined(JS_NUNBOX32)
typedef union jsval_layout
{
    uint64_t asBits;
    struct {
        union {
            int32_t        i32;
            uint32_t       u32;
            uint32_t       boo;     // Don't use |bool| -- it must be four bytes.
            JSString*      str;
            JS::Symbol*    sym;
            JSObject*      obj;
            js::gc::Cell*  cell;
            void*          ptr;
            JSWhyMagic     why;
            size_t         word;
            uintptr_t      uintptr;
        } payload;
        JSValueTag tag;
    } s;
    double asDouble;
    void* asPtr;
} JSVAL_ALIGNMENT jsval_layout;
# elif defined(JS_PUNBOX64)
typedef union jsval_layout
{
    uint64_t asBits;
#if !defined(_WIN64)
    /* MSVC does not pack these correctly :-( */
    struct {
        uint64_t           payload47 : 47;
        JSValueTag         tag : 17;
    } debugView;
#endif
    struct {
        union {
            int32_t        i32;
            uint32_t       u32;
            JSWhyMagic     why;
        } payload;
    } s;
    double asDouble;
    void* asPtr;
    size_t asWord;
    uintptr_t asUIntPtr;
} JSVAL_ALIGNMENT jsval_layout;
# endif  /* JS_PUNBOX64 */
#else   /* defined(IS_LITTLE_ENDIAN) */
# if defined(JS_NUNBOX32)
typedef union jsval_layout
{
    uint64_t asBits;
    struct {
        JSValueTag tag;
        union {
            int32_t        i32;
            uint32_t       u32;
            uint32_t       boo;     // Don't use |bool| -- it must be four bytes.
            JSString*      str;
            JS::Symbol*    sym;
            JSObject*      obj;
            js::gc::Cell*  cell;
            void*          ptr;
            JSWhyMagic     why;
            size_t         word;
            uintptr_t      uintptr;
        } payload;
    } s;
    double asDouble;
    void* asPtr;
} JSVAL_ALIGNMENT jsval_layout;
# elif defined(JS_PUNBOX64)
typedef union jsval_layout
{
    uint64_t asBits;
    struct {
        JSValueTag         tag : 17;
        uint64_t           payload47 : 47;
    } debugView;
    struct {
        uint32_t           padding;
        union {
            int32_t        i32;
            uint32_t       u32;
            JSWhyMagic     why;
        } payload;
    } s;
    double asDouble;
    void* asPtr;
    size_t asWord;
    uintptr_t asUIntPtr;
} JSVAL_ALIGNMENT jsval_layout;
# endif /* JS_PUNBOX64 */
#endif  /* defined(IS_LITTLE_ENDIAN) */

JS_STATIC_ASSERT(sizeof(jsval_layout) == 8);

/*
 * For codesize purposes on some platforms, it's important that the
 * compiler know that JS::Values constructed from constant values can be
 * folded to constant bit patterns at compile time, rather than
 * constructed at runtime.  Doing this requires a fair amount of C++11
 * features, which are not supported on all of our compilers.  Set up
 * some defines and helper macros in an attempt to confine the ugliness
 * here, rather than scattering it all about the file.  The important
 * features are:
 *
 * - constexpr;
 * - defaulted functions;
 * - C99-style designated initializers.
 */
#if defined(__clang__)
#  if __has_feature(cxx_constexpr) && __has_feature(cxx_defaulted_functions)
#    define JS_VALUE_IS_CONSTEXPR
#  endif
#elif defined(__GNUC__)
/*
 * We need 4.5 for defaulted functions, 4.6 for constexpr, 4.7 because 4.6
 * doesn't understand |(X) { .field = ... }| syntax, and 4.7.3 because
 * versions prior to that have bugs in the C++ front-end that cause crashes.
 */
#  if MOZ_GCC_VERSION_AT_LEAST(4, 7, 3)
#    define JS_VALUE_IS_CONSTEXPR
#  endif
#endif

#if defined(JS_VALUE_IS_CONSTEXPR)
#  define JS_RETURN_LAYOUT_FROM_BITS(BITS) \
    return (jsval_layout) { .asBits = (BITS) }
#  define JS_VALUE_CONSTEXPR MOZ_CONSTEXPR
#  define JS_VALUE_CONSTEXPR_VAR MOZ_CONSTEXPR_VAR
#else
#  define JS_RETURN_LAYOUT_FROM_BITS(BITS) \
    jsval_layout l;                        \
    l.asBits = (BITS);                     \
    return l;
#  define JS_VALUE_CONSTEXPR
#  define JS_VALUE_CONSTEXPR_VAR const
#endif

#if defined(JS_NUNBOX32)

/*
 * N.B. GCC, in some but not all cases, chooses to emit signed comparison of
 * JSValueTag even though its underlying type has been forced to be uint32_t.
 * Thus, all comparisons should explicitly cast operands to uint32_t.
 */

static inline JS_VALUE_CONSTEXPR jsval_layout
BUILD_JSVAL(JSValueTag tag, uint32_t payload)
{
    JS_RETURN_LAYOUT_FROM_BITS((((uint64_t)(uint32_t)tag) << 32) | payload);
}

static inline bool
JSVAL_IS_DOUBLE_IMPL(jsval_layout l)
{
    return (uint32_t)l.s.tag <= (uint32_t)JSVAL_TAG_CLEAR;
}

static inline jsval_layout
DOUBLE_TO_JSVAL_IMPL(double d)
{
    jsval_layout l;
    l.asDouble = d;
    MOZ_ASSERT(JSVAL_IS_DOUBLE_IMPL(l));
    return l;
}

static inline bool
JSVAL_IS_INT32_IMPL(jsval_layout l)
{
    return l.s.tag == JSVAL_TAG_INT32;
}

static inline int32_t
JSVAL_TO_INT32_IMPL(jsval_layout l)
{
    return l.s.payload.i32;
}

static inline JS_VALUE_CONSTEXPR jsval_layout
INT32_TO_JSVAL_IMPL(int32_t i)
{
#if defined(JS_VALUE_IS_CONSTEXPR)
    return BUILD_JSVAL(JSVAL_TAG_INT32, i);
#else
    jsval_layout l;
    l.s.tag = JSVAL_TAG_INT32;
    l.s.payload.i32 = i;
    return l;
#endif
}

static inline bool
JSVAL_IS_NUMBER_IMPL(jsval_layout l)
{
    JSValueTag tag = l.s.tag;
    MOZ_ASSERT(tag != JSVAL_TAG_CLEAR);
    return (uint32_t)tag <= (uint32_t)JSVAL_UPPER_INCL_TAG_OF_NUMBER_SET;
}

static inline bool
JSVAL_IS_UNDEFINED_IMPL(jsval_layout l)
{
    return l.s.tag == JSVAL_TAG_UNDEFINED;
}

static inline bool
JSVAL_IS_STRING_IMPL(jsval_layout l)
{
    return l.s.tag == JSVAL_TAG_STRING;
}

static inline jsval_layout
STRING_TO_JSVAL_IMPL(JSString* str)
{
    jsval_layout l;
    MOZ_ASSERT(uintptr_t(str) > 0x1000);
    l.s.tag = JSVAL_TAG_STRING;
    l.s.payload.str = str;
    return l;
}

static inline JSString*
JSVAL_TO_STRING_IMPL(jsval_layout l)
{
    return l.s.payload.str;
}

static inline bool
JSVAL_IS_SYMBOL_IMPL(jsval_layout l)
{
    return l.s.tag == JSVAL_TAG_SYMBOL;
}

static inline jsval_layout
SYMBOL_TO_JSVAL_IMPL(JS::Symbol* sym)
{
    jsval_layout l;
    MOZ_ASSERT(uintptr_t(sym) > 0x1000);
    l.s.tag = JSVAL_TAG_SYMBOL;
    l.s.payload.sym = sym;
    return l;
}

static inline JS::Symbol*
JSVAL_TO_SYMBOL_IMPL(jsval_layout l)
{
    return l.s.payload.sym;
}

static inline bool
JSVAL_IS_BOOLEAN_IMPL(jsval_layout l)
{
    return l.s.tag == JSVAL_TAG_BOOLEAN;
}

static inline bool
JSVAL_TO_BOOLEAN_IMPL(jsval_layout l)
{
    return l.s.payload.boo;
}

static inline jsval_layout
BOOLEAN_TO_JSVAL_IMPL(bool b)
{
    jsval_layout l;
    l.s.tag = JSVAL_TAG_BOOLEAN;
    l.s.payload.boo = b;
    return l;
}

static inline bool
JSVAL_IS_MAGIC_IMPL(jsval_layout l)
{
    return l.s.tag == JSVAL_TAG_MAGIC;
}

static inline bool
JSVAL_IS_OBJECT_IMPL(jsval_layout l)
{
    return l.s.tag == JSVAL_TAG_OBJECT;
}

static inline bool
JSVAL_IS_PRIMITIVE_IMPL(jsval_layout l)
{
    return (uint32_t)l.s.tag < (uint32_t)JSVAL_UPPER_EXCL_TAG_OF_PRIMITIVE_SET;
}

static inline bool
JSVAL_IS_OBJECT_OR_NULL_IMPL(jsval_layout l)
{
    MOZ_ASSERT((uint32_t)l.s.tag <= (uint32_t)JSVAL_TAG_OBJECT);
    return (uint32_t)l.s.tag >= (uint32_t)JSVAL_LOWER_INCL_TAG_OF_OBJ_OR_NULL_SET;
}

static inline JSObject*
JSVAL_TO_OBJECT_IMPL(jsval_layout l)
{
    return l.s.payload.obj;
}

static inline jsval_layout
OBJECT_TO_JSVAL_IMPL(JSObject* obj)
{
    jsval_layout l;
    MOZ_ASSERT(uintptr_t(obj) > 0x1000 || uintptr_t(obj) == 0x42);
    l.s.tag = JSVAL_TAG_OBJECT;
    l.s.payload.obj = obj;
    return l;
}

static inline bool
JSVAL_IS_NULL_IMPL(jsval_layout l)
{
    return l.s.tag == JSVAL_TAG_NULL;
}

static inline jsval_layout
PRIVATE_PTR_TO_JSVAL_IMPL(void* ptr)
{
    jsval_layout l;
    MOZ_ASSERT(((uint32_t)ptr & 1) == 0);
    l.s.tag = (JSValueTag)0;
    l.s.payload.ptr = ptr;
    MOZ_ASSERT(JSVAL_IS_DOUBLE_IMPL(l));
    return l;
}

static inline void*
JSVAL_TO_PRIVATE_PTR_IMPL(jsval_layout l)
{
    return l.s.payload.ptr;
}

static inline bool
JSVAL_IS_GCTHING_IMPL(jsval_layout l)
{
    /* gcc sometimes generates signed < without explicit casts. */
    return (uint32_t)l.s.tag >= (uint32_t)JSVAL_LOWER_INCL_TAG_OF_GCTHING_SET;
}

static inline js::gc::Cell*
JSVAL_TO_GCTHING_IMPL(jsval_layout l)
{
    return l.s.payload.cell;
}

static inline uint32_t
JSVAL_TRACE_KIND_IMPL(jsval_layout l)
{
    static_assert((JSVAL_TAG_STRING & 0x03) == size_t(JS::TraceKind::String),
                  "Value type tags must correspond with JS::TraceKinds.");
    static_assert((JSVAL_TAG_SYMBOL & 0x03) == size_t(JS::TraceKind::Symbol),
                  "Value type tags must correspond with JS::TraceKinds.");
    static_assert((JSVAL_TAG_OBJECT & 0x03) == size_t(JS::TraceKind::Object),
                  "Value type tags must correspond with JS::TraceKinds.");
    return l.s.tag & 0x03;
}

static inline bool
JSVAL_IS_SPECIFIC_INT32_IMPL(jsval_layout l, int32_t i32)
{
    return l.s.tag == JSVAL_TAG_INT32 && l.s.payload.i32 == i32;
}

static inline bool
JSVAL_IS_SPECIFIC_BOOLEAN_IMPL(jsval_layout l, bool b)
{
    return (l.s.tag == JSVAL_TAG_BOOLEAN) && (l.s.payload.boo == uint32_t(b));
}

static inline jsval_layout
MAGIC_TO_JSVAL_IMPL(JSWhyMagic why)
{
    jsval_layout l;
    l.s.tag = JSVAL_TAG_MAGIC;
    l.s.payload.why = why;
    return l;
}

static inline jsval_layout
MAGIC_UINT32_TO_JSVAL_IMPL(uint32_t payload)
{
    jsval_layout l;
    l.s.tag = JSVAL_TAG_MAGIC;
    l.s.payload.u32 = payload;
    return l;
}

static inline bool
JSVAL_SAME_TYPE_IMPL(jsval_layout lhs, jsval_layout rhs)
{
    JSValueTag ltag = lhs.s.tag, rtag = rhs.s.tag;
    return ltag == rtag || (ltag < JSVAL_TAG_CLEAR && rtag < JSVAL_TAG_CLEAR);
}

static inline JSValueType
JSVAL_EXTRACT_NON_DOUBLE_TYPE_IMPL(jsval_layout l)
{
    uint32_t type = l.s.tag & 0xF;
    MOZ_ASSERT(type > JSVAL_TYPE_DOUBLE);
    return (JSValueType)type;
}

#elif defined(JS_PUNBOX64)

static inline JS_VALUE_CONSTEXPR jsval_layout
BUILD_JSVAL(JSValueTag tag, uint64_t payload)
{
    JS_RETURN_LAYOUT_FROM_BITS((((uint64_t)(uint32_t)tag) << JSVAL_TAG_SHIFT) | payload);
}

static inline bool
JSVAL_IS_DOUBLE_IMPL(jsval_layout l)
{
    return l.asBits <= JSVAL_SHIFTED_TAG_MAX_DOUBLE;
}

static inline jsval_layout
DOUBLE_TO_JSVAL_IMPL(double d)
{
    jsval_layout l;
    l.asDouble = d;
    MOZ_ASSERT(l.asBits <= JSVAL_SHIFTED_TAG_MAX_DOUBLE);
    return l;
}

static inline bool
JSVAL_IS_INT32_IMPL(jsval_layout l)
{
    return (uint32_t)(l.asBits >> JSVAL_TAG_SHIFT) == JSVAL_TAG_INT32;
}

static inline int32_t
JSVAL_TO_INT32_IMPL(jsval_layout l)
{
    return (int32_t)l.asBits;
}

static inline JS_VALUE_CONSTEXPR jsval_layout
INT32_TO_JSVAL_IMPL(int32_t i32)
{
    JS_RETURN_LAYOUT_FROM_BITS(((uint64_t)(uint32_t)i32) | JSVAL_SHIFTED_TAG_INT32);
}

static inline bool
JSVAL_IS_NUMBER_IMPL(jsval_layout l)
{
    return l.asBits < JSVAL_UPPER_EXCL_SHIFTED_TAG_OF_NUMBER_SET;
}

static inline bool
JSVAL_IS_UNDEFINED_IMPL(jsval_layout l)
{
    return l.asBits == JSVAL_SHIFTED_TAG_UNDEFINED;
}

static inline bool
JSVAL_IS_STRING_IMPL(jsval_layout l)
{
    return (uint32_t)(l.asBits >> JSVAL_TAG_SHIFT) == JSVAL_TAG_STRING;
}

static inline jsval_layout
STRING_TO_JSVAL_IMPL(JSString* str)
{
    jsval_layout l;
    uint64_t strBits = (uint64_t)str;
    MOZ_ASSERT(uintptr_t(str) > 0x1000);
    MOZ_ASSERT((strBits >> JSVAL_TAG_SHIFT) == 0);
    l.asBits = strBits | JSVAL_SHIFTED_TAG_STRING;
    return l;
}

static inline JSString*
JSVAL_TO_STRING_IMPL(jsval_layout l)
{
    return (JSString*)(l.asBits & JSVAL_PAYLOAD_MASK);
}

static inline bool
JSVAL_IS_SYMBOL_IMPL(jsval_layout l)
{
    return (uint32_t)(l.asBits >> JSVAL_TAG_SHIFT) == JSVAL_TAG_SYMBOL;
}

static inline jsval_layout
SYMBOL_TO_JSVAL_IMPL(JS::Symbol* sym)
{
    jsval_layout l;
    uint64_t symBits = (uint64_t)sym;
    MOZ_ASSERT(uintptr_t(sym) > 0x1000);
    MOZ_ASSERT((symBits >> JSVAL_TAG_SHIFT) == 0);
    l.asBits = symBits | JSVAL_SHIFTED_TAG_SYMBOL;
    return l;
}

static inline JS::Symbol*
JSVAL_TO_SYMBOL_IMPL(jsval_layout l)
{
    return (JS::Symbol*)(l.asBits & JSVAL_PAYLOAD_MASK);
}

static inline bool
JSVAL_IS_BOOLEAN_IMPL(jsval_layout l)
{
    return (uint32_t)(l.asBits >> JSVAL_TAG_SHIFT) == JSVAL_TAG_BOOLEAN;
}

static inline bool
JSVAL_TO_BOOLEAN_IMPL(jsval_layout l)
{
    return (bool)(l.asBits & JSVAL_PAYLOAD_MASK);
}

static inline jsval_layout
BOOLEAN_TO_JSVAL_IMPL(bool b)
{
    jsval_layout l;
    l.asBits = ((uint64_t)(uint32_t)b) | JSVAL_SHIFTED_TAG_BOOLEAN;
    return l;
}

static inline bool
JSVAL_IS_MAGIC_IMPL(jsval_layout l)
{
    return (l.asBits >> JSVAL_TAG_SHIFT) == JSVAL_TAG_MAGIC;
}

static inline bool
JSVAL_IS_PRIMITIVE_IMPL(jsval_layout l)
{
    return l.asBits < JSVAL_UPPER_EXCL_SHIFTED_TAG_OF_PRIMITIVE_SET;
}

static inline bool
JSVAL_IS_OBJECT_IMPL(jsval_layout l)
{
    MOZ_ASSERT((l.asBits >> JSVAL_TAG_SHIFT) <= JSVAL_TAG_OBJECT);
    return l.asBits >= JSVAL_SHIFTED_TAG_OBJECT;
}

static inline bool
JSVAL_IS_OBJECT_OR_NULL_IMPL(jsval_layout l)
{
    MOZ_ASSERT((l.asBits >> JSVAL_TAG_SHIFT) <= JSVAL_TAG_OBJECT);
    return l.asBits >= JSVAL_LOWER_INCL_SHIFTED_TAG_OF_OBJ_OR_NULL_SET;
}

static inline JSObject*
JSVAL_TO_OBJECT_IMPL(jsval_layout l)
{
    uint64_t ptrBits = l.asBits & JSVAL_PAYLOAD_MASK;
    MOZ_ASSERT((ptrBits & 0x7) == 0);
    return (JSObject*)ptrBits;
}

static inline jsval_layout
OBJECT_TO_JSVAL_IMPL(JSObject* obj)
{
    jsval_layout l;
    uint64_t objBits = (uint64_t)obj;
    MOZ_ASSERT(uintptr_t(obj) > 0x1000 || uintptr_t(obj) == 0x42);
    MOZ_ASSERT((objBits >> JSVAL_TAG_SHIFT) == 0);
    l.asBits = objBits | JSVAL_SHIFTED_TAG_OBJECT;
    return l;
}

static inline bool
JSVAL_IS_NULL_IMPL(jsval_layout l)
{
    return l.asBits == JSVAL_SHIFTED_TAG_NULL;
}

static inline bool
JSVAL_IS_GCTHING_IMPL(jsval_layout l)
{
    return l.asBits >= JSVAL_LOWER_INCL_SHIFTED_TAG_OF_GCTHING_SET;
}

static inline js::gc::Cell*
JSVAL_TO_GCTHING_IMPL(jsval_layout l)
{
    uint64_t ptrBits = l.asBits & JSVAL_PAYLOAD_MASK;
    MOZ_ASSERT((ptrBits & 0x7) == 0);
    return reinterpret_cast<js::gc::Cell*>(ptrBits);
}

static inline uint32_t
JSVAL_TRACE_KIND_IMPL(jsval_layout l)
{
    static_assert((JSVAL_TAG_STRING & 0x03) == size_t(JS::TraceKind::String),
                  "Value type tags must correspond with JS::TraceKinds.");
    static_assert((JSVAL_TAG_SYMBOL & 0x03) == size_t(JS::TraceKind::Symbol),
                  "Value type tags must correspond with JS::TraceKinds.");
    static_assert((JSVAL_TAG_OBJECT & 0x03) == size_t(JS::TraceKind::Object),
                  "Value type tags must correspond with JS::TraceKinds.");
    return (uint32_t)(l.asBits >> JSVAL_TAG_SHIFT) & 0x03;
}

static inline jsval_layout
PRIVATE_PTR_TO_JSVAL_IMPL(void* ptr)
{
    jsval_layout l;
    uint64_t ptrBits = (uint64_t)ptr;
    MOZ_ASSERT((ptrBits & 1) == 0);
    l.asBits = ptrBits >> 1;
    MOZ_ASSERT(JSVAL_IS_DOUBLE_IMPL(l));
    return l;
}

static inline void*
JSVAL_TO_PRIVATE_PTR_IMPL(jsval_layout l)
{
    MOZ_ASSERT((l.asBits & 0x8000000000000000LL) == 0);
    return (void*)(l.asBits << 1);
}

static inline bool
JSVAL_IS_SPECIFIC_INT32_IMPL(jsval_layout l, int32_t i32)
{
    return l.asBits == (((uint64_t)(uint32_t)i32) | JSVAL_SHIFTED_TAG_INT32);
}

static inline bool
JSVAL_IS_SPECIFIC_BOOLEAN_IMPL(jsval_layout l, bool b)
{
    return l.asBits == (((uint64_t)(uint32_t)b) | JSVAL_SHIFTED_TAG_BOOLEAN);
}

static inline jsval_layout
MAGIC_TO_JSVAL_IMPL(JSWhyMagic why)
{
    jsval_layout l;
    l.asBits = ((uint64_t)(uint32_t)why) | JSVAL_SHIFTED_TAG_MAGIC;
    return l;
}

static inline jsval_layout
MAGIC_UINT32_TO_JSVAL_IMPL(uint32_t payload)
{
    jsval_layout l;
    l.asBits = ((uint64_t)payload) | JSVAL_SHIFTED_TAG_MAGIC;
    return l;
}

static inline bool
JSVAL_SAME_TYPE_IMPL(jsval_layout lhs, jsval_layout rhs)
{
    uint64_t lbits = lhs.asBits, rbits = rhs.asBits;
    return (lbits <= JSVAL_SHIFTED_TAG_MAX_DOUBLE && rbits <= JSVAL_SHIFTED_TAG_MAX_DOUBLE) ||
           (((lbits ^ rbits) & 0xFFFF800000000000LL) == 0);
}

static inline JSValueType
JSVAL_EXTRACT_NON_DOUBLE_TYPE_IMPL(jsval_layout l)
{
   uint64_t type = (l.asBits >> JSVAL_TAG_SHIFT) & 0xF;
   MOZ_ASSERT(type > JSVAL_TYPE_DOUBLE);
   return (JSValueType)type;
}

#endif  /* JS_PUNBOX64 */

static inline bool
JSVAL_IS_TRACEABLE_IMPL(jsval_layout l)
{
    return JSVAL_IS_GCTHING_IMPL(l) && !JSVAL_IS_NULL_IMPL(l);
}

static inline jsval_layout JSVAL_TO_IMPL(JS::Value v);
static inline JS_VALUE_CONSTEXPR JS::Value IMPL_TO_JSVAL(jsval_layout l);

namespace JS {

static inline JS_VALUE_CONSTEXPR JS::Value UndefinedValue();

/**
 * Returns a generic quiet NaN value, with all payload bits set to zero.
 *
 * Among other properties, this NaN's bit pattern conforms to JS::Value's
 * bit pattern restrictions.
 */
static MOZ_ALWAYS_INLINE double
GenericNaN()
{
  return mozilla::SpecificNaN<double>(0, 0x8000000000000ULL);
}

/* MSVC with PGO miscompiles this function. */
#if defined(_MSC_VER)
# pragma optimize("g", off)
#endif
static inline double
CanonicalizeNaN(double d)
{
    if (MOZ_UNLIKELY(mozilla::IsNaN(d)))
        return GenericNaN();
    return d;
}
#if defined(_MSC_VER)
# pragma optimize("", on)
#endif

/**
 * JS::Value is the interface for a single JavaScript Engine value.  A few
 * general notes on JS::Value:
 *
 * - JS::Value has setX() and isX() members for X in
 *
 *     { Int32, Double, String, Symbol, Boolean, Undefined, Null, Object, Magic }
 *
 *   JS::Value also contains toX() for each of the non-singleton types.
 *
 * - Magic is a singleton type whose payload contains either a JSWhyMagic "reason" for
 *   the magic value or a uint32_t value. By providing JSWhyMagic values when
 *   creating and checking for magic values, it is possible to assert, at
 *   runtime, that only magic values with the expected reason flow through a
 *   particular value. For example, if cx->exception has a magic value, the
 *   reason must be JS_GENERATOR_CLOSING.
 *
 * - The JS::Value operations are preferred.  The JSVAL_* operations remain for
 *   compatibility; they may be removed at some point.  These operations mostly
 *   provide similar functionality.  But there are a few key differences.  One
 *   is that JS::Value gives null a separate type.
 *   Also, to help prevent mistakenly boxing a nullable JSObject* as an object,
 *   Value::setObject takes a JSObject&. (Conversely, Value::toObject returns a
 *   JSObject&.)  A convenience member Value::setObjectOrNull is provided.
 *
 * - JSVAL_VOID is the same as the singleton value of the Undefined type.
 *
 * - Note that JS::Value is 8 bytes on 32 and 64-bit architectures. Thus, on
 *   32-bit user code should avoid copying jsval/JS::Value as much as possible,
 *   preferring to pass by const Value&.
 */
class Value
{
  public:
    /*
     * N.B. the default constructor leaves Value unitialized. Adding a default
     * constructor prevents Value from being stored in a union.
     */
#if defined(JS_VALUE_IS_CONSTEXPR)
    Value() = default;
    Value(const Value& v) = default;
#endif

    /**
     * Returns false if creating a NumberValue containing the given type would
     * be lossy, true otherwise.
     */
    template <typename T>
    static bool isNumberRepresentable(const T t) {
        return T(double(t)) == t;
    }

    /*** Mutators ***/

    void setNull() {
        data.asBits = BUILD_JSVAL(JSVAL_TAG_NULL, 0).asBits;
    }

    void setUndefined() {
        data.asBits = BUILD_JSVAL(JSVAL_TAG_UNDEFINED, 0).asBits;
    }

    void setInt32(int32_t i) {
        data = INT32_TO_JSVAL_IMPL(i);
    }

    int32_t& getInt32Ref() {
        MOZ_ASSERT(isInt32());
        return data.s.payload.i32;
    }

    void setDouble(double d) {
        data = DOUBLE_TO_JSVAL_IMPL(d);
    }

    void setNaN() {
        setDouble(GenericNaN());
    }

    double& getDoubleRef() {
        MOZ_ASSERT(isDouble());
        return data.asDouble;
    }

    void setString(JSString* str) {
        data = STRING_TO_JSVAL_IMPL(str);
    }

    void setSymbol(JS::Symbol* sym) {
        data = SYMBOL_TO_JSVAL_IMPL(sym);
    }

    void setObject(JSObject& obj) {
        data = OBJECT_TO_JSVAL_IMPL(&obj);
    }

    void setBoolean(bool b) {
        data = BOOLEAN_TO_JSVAL_IMPL(b);
    }

    void setMagic(JSWhyMagic why) {
        data = MAGIC_TO_JSVAL_IMPL(why);
    }

    void setMagicUint32(uint32_t payload) {
        data = MAGIC_UINT32_TO_JSVAL_IMPL(payload);
    }

    bool setNumber(uint32_t ui) {
        if (ui > JSVAL_INT_MAX) {
            setDouble((double)ui);
            return false;
        } else {
            setInt32((int32_t)ui);
            return true;
        }
    }

    bool setNumber(double d) {
        int32_t i;
        if (mozilla::NumberIsInt32(d, &i)) {
            setInt32(i);
            return true;
        }

        setDouble(d);
        return false;
    }

    void setObjectOrNull(JSObject* arg) {
        if (arg)
            setObject(*arg);
        else
            setNull();
    }

    void swap(Value& rhs) {
        uint64_t tmp = rhs.data.asBits;
        rhs.data.asBits = data.asBits;
        data.asBits = tmp;
    }

    /*** Value type queries ***/

    bool isUndefined() const {
        return JSVAL_IS_UNDEFINED_IMPL(data);
    }

    bool isNull() const {
        return JSVAL_IS_NULL_IMPL(data);
    }

    bool isNullOrUndefined() const {
        return isNull() || isUndefined();
    }

    bool isInt32() const {
        return JSVAL_IS_INT32_IMPL(data);
    }

    bool isInt32(int32_t i32) const {
        return JSVAL_IS_SPECIFIC_INT32_IMPL(data, i32);
    }

    bool isDouble() const {
        return JSVAL_IS_DOUBLE_IMPL(data);
    }

    bool isNumber() const {
        return JSVAL_IS_NUMBER_IMPL(data);
    }

    bool isString() const {
        return JSVAL_IS_STRING_IMPL(data);
    }

    bool isSymbol() const {
        return JSVAL_IS_SYMBOL_IMPL(data);
    }

    bool isObject() const {
        return JSVAL_IS_OBJECT_IMPL(data);
    }

    bool isPrimitive() const {
        return JSVAL_IS_PRIMITIVE_IMPL(data);
    }

    bool isObjectOrNull() const {
        return JSVAL_IS_OBJECT_OR_NULL_IMPL(data);
    }

    bool isGCThing() const {
        return JSVAL_IS_GCTHING_IMPL(data);
    }

    bool isBoolean() const {
        return JSVAL_IS_BOOLEAN_IMPL(data);
    }

    bool isTrue() const {
        return JSVAL_IS_SPECIFIC_BOOLEAN_IMPL(data, true);
    }

    bool isFalse() const {
        return JSVAL_IS_SPECIFIC_BOOLEAN_IMPL(data, false);
    }

    bool isMagic() const {
        return JSVAL_IS_MAGIC_IMPL(data);
    }

    bool isMagic(JSWhyMagic why) const {
        MOZ_ASSERT_IF(isMagic(), data.s.payload.why == why);
        return JSVAL_IS_MAGIC_IMPL(data);
    }

    bool isMarkable() const {
        return JSVAL_IS_TRACEABLE_IMPL(data);
    }

    JS::TraceKind traceKind() const {
        MOZ_ASSERT(isMarkable());
        return JS::TraceKind(JSVAL_TRACE_KIND_IMPL(data));
    }

    JSWhyMagic whyMagic() const {
        MOZ_ASSERT(isMagic());
        return data.s.payload.why;
    }

    uint32_t magicUint32() const {
        MOZ_ASSERT(isMagic());
        return data.s.payload.u32;
    }

    /*** Comparison ***/

    bool operator==(const Value& rhs) const {
        return data.asBits == rhs.data.asBits;
    }

    bool operator!=(const Value& rhs) const {
        return data.asBits != rhs.data.asBits;
    }

    friend inline bool SameType(const Value& lhs, const Value& rhs);

    /*** Extract the value's typed payload ***/

    int32_t toInt32() const {
        MOZ_ASSERT(isInt32());
        return JSVAL_TO_INT32_IMPL(data);
    }

    double toDouble() const {
        MOZ_ASSERT(isDouble());
        return data.asDouble;
    }

    double toNumber() const {
        MOZ_ASSERT(isNumber());
        return isDouble() ? toDouble() : double(toInt32());
    }

    JSString* toString() const {
        MOZ_ASSERT(isString());
        return JSVAL_TO_STRING_IMPL(data);
    }

    JS::Symbol* toSymbol() const {
        MOZ_ASSERT(isSymbol());
        return JSVAL_TO_SYMBOL_IMPL(data);
    }

    JSObject& toObject() const {
        MOZ_ASSERT(isObject());
        return *JSVAL_TO_OBJECT_IMPL(data);
    }

    JSObject* toObjectOrNull() const {
        MOZ_ASSERT(isObjectOrNull());
        return JSVAL_TO_OBJECT_IMPL(data);
    }

    js::gc::Cell* toGCThing() const {
        MOZ_ASSERT(isGCThing());
        return JSVAL_TO_GCTHING_IMPL(data);
    }

    GCCellPtr toGCCellPtr() const {
        return GCCellPtr(toGCThing(), traceKind());
    }

    bool toBoolean() const {
        MOZ_ASSERT(isBoolean());
        return JSVAL_TO_BOOLEAN_IMPL(data);
    }

    uint32_t payloadAsRawUint32() const {
        MOZ_ASSERT(!isDouble());
        return data.s.payload.u32;
    }

    uint64_t asRawBits() const {
        return data.asBits;
    }

    JSValueType extractNonDoubleType() const {
        return JSVAL_EXTRACT_NON_DOUBLE_TYPE_IMPL(data);
    }

    /*
     * Private API
     *
     * Private setters/getters allow the caller to read/write arbitrary types
     * that fit in the 64-bit payload. It is the caller's responsibility, after
     * storing to a value with setPrivateX to read only using getPrivateX.
     * Privates values are given a type which ensures they are not marked.
     */

    void setPrivate(void* ptr) {
        data = PRIVATE_PTR_TO_JSVAL_IMPL(ptr);
    }

    void* toPrivate() const {
        MOZ_ASSERT(JSVAL_IS_DOUBLE_IMPL(data));
        return JSVAL_TO_PRIVATE_PTR_IMPL(data);
    }

    void setPrivateUint32(uint32_t ui) {
        MOZ_ASSERT(uint32_t(int32_t(ui)) == ui);
        setInt32(int32_t(ui));
    }

    uint32_t toPrivateUint32() const {
        return uint32_t(toInt32());
    }

    /*
     * An unmarked value is just a void* cast as a Value. Thus, the Value is
     * not safe for GC and must not be marked. This API avoids raw casts
     * and the ensuing strict-aliasing warnings.
     */

    void setUnmarkedPtr(void* ptr) {
        data.asPtr = ptr;
    }

    void* toUnmarkedPtr() const {
        return data.asPtr;
    }

    const size_t* payloadWord() const {
#if defined(JS_NUNBOX32)
        return &data.s.payload.word;
#elif defined(JS_PUNBOX64)
        return &data.asWord;
#endif
    }

    const uintptr_t* payloadUIntPtr() const {
#if defined(JS_NUNBOX32)
        return &data.s.payload.uintptr;
#elif defined(JS_PUNBOX64)
        return &data.asUIntPtr;
#endif
    }

#if !defined(_MSC_VER) && !defined(__sparc)
  // Value must be POD so that MSVC will pass it by value and not in memory
  // (bug 689101); the same is true for SPARC as well (bug 737344).  More
  // precisely, we don't want Value return values compiled as out params.
  private:
#endif

    jsval_layout data;

  private:
#if defined(JS_VALUE_IS_CONSTEXPR)
    MOZ_IMPLICIT JS_VALUE_CONSTEXPR Value(jsval_layout layout) : data(layout) {}
#endif

    void staticAssertions() {
        JS_STATIC_ASSERT(sizeof(JSValueType) == 1);
        JS_STATIC_ASSERT(sizeof(JSValueTag) == 4);
        JS_STATIC_ASSERT(sizeof(JSWhyMagic) <= 4);
        JS_STATIC_ASSERT(sizeof(Value) == 8);
    }

    friend jsval_layout (::JSVAL_TO_IMPL)(Value);
    friend Value JS_VALUE_CONSTEXPR (::IMPL_TO_JSVAL)(jsval_layout l);
    friend Value JS_VALUE_CONSTEXPR (JS::UndefinedValue)();
};

inline bool
IsOptimizedPlaceholderMagicValue(const Value& v)
{
    if (v.isMagic()) {
        MOZ_ASSERT(v.whyMagic() == JS_OPTIMIZED_ARGUMENTS || v.whyMagic() == JS_OPTIMIZED_OUT);
        return true;
    }
    return false;
}

static MOZ_ALWAYS_INLINE void
ExposeValueToActiveJS(const Value& v)
{
    if (v.isMarkable())
        js::gc::ExposeGCThingToActiveJS(GCCellPtr(v));
}

/************************************************************************/

static inline Value
NullValue()
{
    Value v;
    v.setNull();
    return v;
}

static inline JS_VALUE_CONSTEXPR Value
UndefinedValue()
{
#if defined(JS_VALUE_IS_CONSTEXPR)
    return Value(BUILD_JSVAL(JSVAL_TAG_UNDEFINED, 0));
#else
    JS::Value v;
    v.setUndefined();
    return v;
#endif
}

static inline JS_VALUE_CONSTEXPR Value
Int32Value(int32_t i32)
{
    return IMPL_TO_JSVAL(INT32_TO_JSVAL_IMPL(i32));
}

static inline Value
DoubleValue(double dbl)
{
    Value v;
    v.setDouble(dbl);
    return v;
}

static inline JS_VALUE_CONSTEXPR Value
CanonicalizedDoubleValue(double d)
{
    /*
     * This is a manually inlined version of:
     *    d = JS_CANONICALIZE_NAN(d);
     *    return IMPL_TO_JSVAL(DOUBLE_TO_JSVAL_IMPL(d));
     * because GCC from XCode 3.1.4 miscompiles the above code.
     */
#if defined(JS_VALUE_IS_CONSTEXPR)
    return IMPL_TO_JSVAL(MOZ_UNLIKELY(mozilla::IsNaN(d))
                         ? (jsval_layout) { .asBits = 0x7FF8000000000000LL }
                         : (jsval_layout) { .asDouble = d });
#else
    jsval_layout l;
    if (MOZ_UNLIKELY(d != d))
        l.asBits = 0x7FF8000000000000LL;
    else
        l.asDouble = d;
    return IMPL_TO_JSVAL(l);
#endif
}

static inline Value
DoubleNaNValue()
{
    Value v;
    v.setNaN();
    return v;
}

static inline Value
Float32Value(float f)
{
    Value v;
    v.setDouble(f);
    return v;
}

static inline Value
StringValue(JSString* str)
{
    Value v;
    v.setString(str);
    return v;
}

static inline Value
SymbolValue(JS::Symbol* sym)
{
    Value v;
    v.setSymbol(sym);
    return v;
}

static inline Value
BooleanValue(bool boo)
{
    Value v;
    v.setBoolean(boo);
    return v;
}

static inline Value
TrueValue()
{
    Value v;
    v.setBoolean(true);
    return v;
}

static inline Value
FalseValue()
{
    Value v;
    v.setBoolean(false);
    return v;
}

static inline Value
ObjectValue(JSObject& obj)
{
    Value v;
    v.setObject(obj);
    return v;
}

static inline Value
ObjectValueCrashOnTouch()
{
    Value v;
    v.setObject(*reinterpret_cast<JSObject*>(0x42));
    return v;
}

static inline Value
MagicValue(JSWhyMagic why)
{
    Value v;
    v.setMagic(why);
    return v;
}

static inline Value
MagicValueUint32(uint32_t payload)
{
    Value v;
    v.setMagicUint32(payload);
    return v;
}

static inline Value
NumberValue(float f)
{
    Value v;
    v.setNumber(f);
    return v;
}

static inline Value
NumberValue(double dbl)
{
    Value v;
    v.setNumber(dbl);
    return v;
}

static inline Value
NumberValue(int8_t i)
{
    return Int32Value(i);
}

static inline Value
NumberValue(uint8_t i)
{
    return Int32Value(i);
}

static inline Value
NumberValue(int16_t i)
{
    return Int32Value(i);
}

static inline Value
NumberValue(uint16_t i)
{
    return Int32Value(i);
}

static inline Value
NumberValue(int32_t i)
{
    return Int32Value(i);
}

static inline JS_VALUE_CONSTEXPR Value
NumberValue(uint32_t i)
{
    return i <= JSVAL_INT_MAX
           ? Int32Value(int32_t(i))
           : CanonicalizedDoubleValue(double(i));
}

namespace detail {

template <bool Signed>
class MakeNumberValue
{
  public:
    template<typename T>
    static inline Value create(const T t)
    {
        Value v;
        if (JSVAL_INT_MIN <= t && t <= JSVAL_INT_MAX)
            v.setInt32(int32_t(t));
        else
            v.setDouble(double(t));
        return v;
    }
};

template <>
class MakeNumberValue<false>
{
  public:
    template<typename T>
    static inline Value create(const T t)
    {
        Value v;
        if (t <= JSVAL_INT_MAX)
            v.setInt32(int32_t(t));
        else
            v.setDouble(double(t));
        return v;
    }
};

} // namespace detail

template <typename T>
static inline Value
NumberValue(const T t)
{
    MOZ_ASSERT(Value::isNumberRepresentable(t), "value creation would be lossy");
    return detail::MakeNumberValue<std::numeric_limits<T>::is_signed>::create(t);
}

static inline Value
ObjectOrNullValue(JSObject* obj)
{
    Value v;
    v.setObjectOrNull(obj);
    return v;
}

static inline Value
PrivateValue(void* ptr)
{
    Value v;
    v.setPrivate(ptr);
    return v;
}

static inline Value
PrivateUint32Value(uint32_t ui)
{
    Value v;
    v.setPrivateUint32(ui);
    return v;
}

inline bool
SameType(const Value& lhs, const Value& rhs)
{
    return JSVAL_SAME_TYPE_IMPL(lhs.data, rhs.data);
}

} // namespace JS

/************************************************************************/

namespace JS {
JS_PUBLIC_API(void) HeapValuePostBarrier(Value* valuep, const Value& prev, const Value& next);
} // namespace JS

namespace js {

template <> struct GCMethods<const JS::Value>
{
    static JS::Value initial() { return JS::UndefinedValue(); }
};

template <> struct GCMethods<JS::Value>
{
    static JS::Value initial() { return JS::UndefinedValue(); }
    static gc::Cell* asGCThingOrNull(const JS::Value& v) {
        return v.isMarkable() ? v.toGCThing() : nullptr;
    }
    static void postBarrier(JS::Value* v, const JS::Value& prev, const JS::Value& next) {
        JS::HeapValuePostBarrier(v, prev, next);
    }
};

template <class Outer> class MutableValueOperations;

/**
 * A class designed for CRTP use in implementing the non-mutating parts of the
 * Value interface in Value-like classes.  Outer must be a class inheriting
 * ValueOperations<Outer> with a visible get() method returning a const
 * reference to the Value abstracted by Outer.
 */
template <class Outer>
class ValueOperations
{
    friend class MutableValueOperations<Outer>;

    const JS::Value& value() const { return static_cast<const Outer*>(this)->get(); }

  public:
    bool isUndefined() const { return value().isUndefined(); }
    bool isNull() const { return value().isNull(); }
    bool isBoolean() const { return value().isBoolean(); }
    bool isTrue() const { return value().isTrue(); }
    bool isFalse() const { return value().isFalse(); }
    bool isNumber() const { return value().isNumber(); }
    bool isInt32() const { return value().isInt32(); }
    bool isInt32(int32_t i32) const { return value().isInt32(i32); }
    bool isDouble() const { return value().isDouble(); }
    bool isString() const { return value().isString(); }
    bool isSymbol() const { return value().isSymbol(); }
    bool isObject() const { return value().isObject(); }
    bool isMagic() const { return value().isMagic(); }
    bool isMagic(JSWhyMagic why) const { return value().isMagic(why); }
    bool isMarkable() const { return value().isMarkable(); }
    bool isPrimitive() const { return value().isPrimitive(); }
    bool isGCThing() const { return value().isGCThing(); }

    bool isNullOrUndefined() const { return value().isNullOrUndefined(); }
    bool isObjectOrNull() const { return value().isObjectOrNull(); }

    bool toBoolean() const { return value().toBoolean(); }
    double toNumber() const { return value().toNumber(); }
    int32_t toInt32() const { return value().toInt32(); }
    double toDouble() const { return value().toDouble(); }
    JSString* toString() const { return value().toString(); }
    JS::Symbol* toSymbol() const { return value().toSymbol(); }
    JSObject& toObject() const { return value().toObject(); }
    JSObject* toObjectOrNull() const { return value().toObjectOrNull(); }
    gc::Cell* toGCThing() const { return value().toGCThing(); }
    JS::TraceKind traceKind() const { return value().traceKind(); }
    uint64_t asRawBits() const { return value().asRawBits(); }

    JSValueType extractNonDoubleType() const { return value().extractNonDoubleType(); }
    uint32_t toPrivateUint32() const { return value().toPrivateUint32(); }

    JSWhyMagic whyMagic() const { return value().whyMagic(); }
    uint32_t magicUint32() const { return value().magicUint32(); }
};

/**
 * A class designed for CRTP use in implementing all the mutating parts of the
 * Value interface in Value-like classes.  Outer must be a class inheriting
 * MutableValueOperations<Outer> with visible get() methods returning const and
 * non-const references to the Value abstracted by Outer.
 */
template <class Outer>
class MutableValueOperations : public ValueOperations<Outer>
{
    JS::Value& value() { return static_cast<Outer*>(this)->get(); }

  public:
    void setNull() { value().setNull(); }
    void setUndefined() { value().setUndefined(); }
    void setInt32(int32_t i) { value().setInt32(i); }
    void setDouble(double d) { value().setDouble(d); }
    void setNaN() { setDouble(JS::GenericNaN()); }
    void setBoolean(bool b) { value().setBoolean(b); }
    void setMagic(JSWhyMagic why) { value().setMagic(why); }
    bool setNumber(uint32_t ui) { return value().setNumber(ui); }
    bool setNumber(double d) { return value().setNumber(d); }
    void setString(JSString* str) { this->value().setString(str); }
    void setSymbol(JS::Symbol* sym) { this->value().setSymbol(sym); }
    void setObject(JSObject& obj) { this->value().setObject(obj); }
    void setObjectOrNull(JSObject* arg) { this->value().setObjectOrNull(arg); }
};

/*
 * Augment the generic Heap<T> interface when T = Value with
 * type-querying, value-extracting, and mutating operations.
 */
template <>
class HeapBase<JS::Value> : public ValueOperations<JS::Heap<JS::Value> >
{
    typedef JS::Heap<JS::Value> Outer;

    friend class ValueOperations<Outer>;

    void setBarriered(const JS::Value& v) {
        *static_cast<JS::Heap<JS::Value>*>(this) = v;
    }

  public:
    void setNull() { setBarriered(JS::NullValue()); }
    void setUndefined() { setBarriered(JS::UndefinedValue()); }
    void setInt32(int32_t i) { setBarriered(JS::Int32Value(i)); }
    void setDouble(double d) { setBarriered(JS::DoubleValue(d)); }
    void setNaN() { setDouble(JS::GenericNaN()); }
    void setBoolean(bool b) { setBarriered(JS::BooleanValue(b)); }
    void setMagic(JSWhyMagic why) { setBarriered(JS::MagicValue(why)); }
    void setString(JSString* str) { setBarriered(JS::StringValue(str)); }
    void setSymbol(JS::Symbol* sym) { setBarriered(JS::SymbolValue(sym)); }
    void setObject(JSObject& obj) { setBarriered(JS::ObjectValue(obj)); }

    bool setNumber(uint32_t ui) {
        if (ui > JSVAL_INT_MAX) {
            setDouble((double)ui);
            return false;
        } else {
            setInt32((int32_t)ui);
            return true;
        }
    }

    bool setNumber(double d) {
        int32_t i;
        if (mozilla::NumberIsInt32(d, &i)) {
            setInt32(i);
            return true;
        }

        setDouble(d);
        return false;
    }

    void setObjectOrNull(JSObject* arg) {
        if (arg)
            setObject(*arg);
        else
            setNull();
    }
};

template <>
class HandleBase<JS::Value> : public ValueOperations<JS::Handle<JS::Value> >
{};

template <>
class MutableHandleBase<JS::Value> : public MutableValueOperations<JS::MutableHandle<JS::Value> >
{};

template <>
class RootedBase<JS::Value> : public MutableValueOperations<JS::Rooted<JS::Value> >
{};

template <>
class PersistentRootedBase<JS::Value> : public MutableValueOperations<JS::PersistentRooted<JS::Value>>
{};

/*
 * If the Value is a GC pointer type, convert to that type and call |f| with
 * the pointer. If the Value is not a GC type, calls F::defaultValue.
 */
template <typename F, typename... Args>
auto
DispatchTyped(F f, const JS::Value& val, Args&&... args)
  -> decltype(f(static_cast<JSObject*>(nullptr), mozilla::Forward<Args>(args)...))
{
    if (val.isString())
        return f(val.toString(), mozilla::Forward<Args>(args)...);
    if (val.isObject())
        return f(&val.toObject(), mozilla::Forward<Args>(args)...);
    if (val.isSymbol())
        return f(val.toSymbol(), mozilla::Forward<Args>(args)...);
    MOZ_ASSERT(!val.isMarkable());
    return F::defaultValue(val);
}

template <class S> struct VoidDefaultAdaptor { static void defaultValue(S) {} };
template <class S> struct IdentityDefaultAdaptor { static S defaultValue(const S& v) {return v;} };
template <class S, bool v> struct BoolDefaultAdaptor { static bool defaultValue(S) { return v; } };

} // namespace js

inline jsval_layout
JSVAL_TO_IMPL(JS::Value v)
{
    return v.data;
}

inline JS_VALUE_CONSTEXPR JS::Value
IMPL_TO_JSVAL(jsval_layout l)
{
#if defined(JS_VALUE_IS_CONSTEXPR)
    return JS::Value(l);
#else
    JS::Value v;
    v.data = l;
    return v;
#endif
}

namespace JS {

#ifdef JS_DEBUG
namespace detail {

struct ValueAlignmentTester { char c; JS::Value v; };
static_assert(sizeof(ValueAlignmentTester) == 16,
              "JS::Value must be 16-byte-aligned");

struct LayoutAlignmentTester { char c; jsval_layout l; };
static_assert(sizeof(LayoutAlignmentTester) == 16,
              "jsval_layout must be 16-byte-aligned");

} // namespace detail
#endif /* JS_DEBUG */

} // namespace JS

static_assert(sizeof(jsval_layout) == sizeof(JS::Value),
              "jsval_layout and JS::Value must have identical layouts");

/************************************************************************/

namespace JS {

extern JS_PUBLIC_DATA(const HandleValue) NullHandleValue;
extern JS_PUBLIC_DATA(const HandleValue) UndefinedHandleValue;
extern JS_PUBLIC_DATA(const HandleValue) TrueHandleValue;
extern JS_PUBLIC_DATA(const HandleValue) FalseHandleValue;

} // namespace JS

#undef JS_VALUE_IS_CONSTEXPR
#undef JS_RETURN_LAYOUT_FROM_BITS

#endif /* js_Value_h */
