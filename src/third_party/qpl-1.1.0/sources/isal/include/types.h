/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 *  @file  types.h
 *  @brief Defines standard width types.
 *
 */

#ifndef __TYPES_H
#define __TYPES_H

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#ifdef __MINGW32__
# include <_mingw.h>
#endif
#endif


#if defined  __unix__ || defined __APPLE__
# define DECLARE_ALIGNED(decl, alignval) decl __attribute__((aligned(alignval)))
# define __forceinline static inline
# define aligned_free(x) free(x)
#else
# ifdef __MINGW32__
#   define DECLARE_ALIGNED(decl, alignval) decl __attribute__((aligned(alignval)))
#   define posix_memalign(p, algn, len) (NULL == (*((char**)(p)) = (void*) _aligned_malloc(len, algn)))
#   define aligned_free(x) _aligned_free(x)
# else
#   define DECLARE_ALIGNED(decl, alignval) __declspec(align(alignval)) decl
#   define posix_memalign(p, algn, len) (NULL == (*((char**)(p)) = (void*) _aligned_malloc(len, algn)))
#   define aligned_free(x) _aligned_free(x)
# endif
#endif

#ifdef DEBUG
# define DEBUG_PRINT(x) printf x
#else
# define DEBUG_PRINT(x) do {} while (0)
#endif

#ifdef __cplusplus
}
#endif

#endif  //__TYPES_H
