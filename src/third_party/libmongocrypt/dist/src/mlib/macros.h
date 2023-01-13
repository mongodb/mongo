#ifndef MLIB_MACROS_H_INCLUDED
#define MLIB_MACROS_H_INCLUDED

#include "./user-check.h"

/**
 * @brief Cross-C/C++ compatibility for a compound initializer to be treated as
 * a braced initializer
 *
 */
#ifdef __cplusplus
#define MLIB_INIT(T) T
#else
#define MLIB_INIT(T) (T)
#endif

#ifdef __cplusplus
#define _mlibCLinkageBegin extern "C" {
#define _mlibCLinkageEnd }
#else
#define _mlibCLinkageBegin
#define _mlibCLinkageEnd
#endif

/// Mark the beginning of a C-language-linkage section
#define MLIB_C_LINKAGE_BEGIN _mlibCLinkageBegin
/// End a C-language-linkage section
#define MLIB_C_LINKAGE_END _mlibCLinkageEnd

#if (defined(__cpp_constexpr) && __cpp_constexpr >= 201304L) || \
   (defined(__cplusplus) && __cplusplus >= 201402L) ||          \
   (defined(_MSVC_LANG) && _MSVC_LANG >= 201402L)
#define _mlibConstexprFn constexpr inline
#else
#define _mlibConstexprFn inline
#endif

/**
 * @brief Mark a function as constexpr
 *
 * Expands to `constexpr inline` in C++14 and above (and someday C26...?).
 * "inline" otherwise.
 */
#define mlib_constexpr_fn _mlibConstexprFn

#endif // MLIB_MACROS_H_INCLUDED
