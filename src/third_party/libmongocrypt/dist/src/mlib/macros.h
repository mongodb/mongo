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

#endif // MLIB_MACROS_H_INCLUDED
