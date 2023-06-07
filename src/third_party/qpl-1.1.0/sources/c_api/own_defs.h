/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef OWN_DEFS_H__
#define OWN_DEFS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "qpl/c_api/defs.h"
#include <igzip_lib.h>
#include "own_checkers.h"

#if defined(__INTEL_COMPILER) || defined(_MSC_VER)
#define QPL_INLINE static __inline
#elif defined( __GNUC__ )
#define QPL_INLINE static __inline__
#else
#define QPL_INLINE static
#endif

#ifndef QPL_UNREFERENCED_PARAMETER
#define QPL_UNREFERENCED_PARAMETER(p) (p)=(p)
#endif

QPL_INLINE int64_t QPL_INT_PTR(const void *ptr) {
    union {
        void    *ptr;
        int64_t int_value;
    } dd;
    dd.ptr = (void *) ptr;
    return dd.int_value;
}

#define OWN_BYTE_BIT_LEN              8u
#define OWN_HIGH_BIT_MASK             0x80u
#define OWN_LOW_BIT_MASK              1u
#define OWN_MAX_BIT_IDX               7u
#define OWN_64U_BITS                  64u

#define QPL_BYTES_TO_ALIGN(ptr, align) ((-(QPL_INT_PTR(ptr)&((align)-1)))&((align)-1))
#define QPL_ALIGNED_PTR(ptr, align) (void*)( (uint8_t*)(ptr) + (QPL_BYTES_TO_ALIGN( ptr, align )) )

#define QPL_ALIGNED_SIZE(size, align) (((size)+(align)-1)&~((align)-1))

#define QPL_DEFAULT_ALIGNMENT 64u

#define QPL_FUN(type, name, arg) extern type name arg

#define OWN_FUN(type, name, arg) extern type name arg

#define OWN_MAX_16U       0xFFFF                      /**< Max value for uint16_t */
#define OWN_MAX_32U       0xFFFFFFFF                  /**< Max value for uint32_t */
#define OWN_BYTE_WIDTH    8u                          /**< Byte width in bits */


#define HUFF_LOOK_UP_TABLE_SIZE UINT16_MAX

#ifdef __cplusplus
}
#endif

#endif // OWN_DEFS_H__
