/* clang-format off */
/* clang-format gets confused by the #define bool line, and gives crazy indenting */
#ifndef AWS_COMMON_STDBOOL_H
#define AWS_COMMON_STDBOOL_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#ifndef NO_STDBOOL
#    include <stdbool.h> /* NOLINT(fuchsia-restrict-system-includes) */
#else
#    ifndef __cplusplus
#        define bool _Bool
#        define true 1
#        define false 0
#    elif defined(__GNUC__) && !defined(__STRICT_ANSI__)
#        define _Bool bool
#        if __cplusplus < 201103L
/* For C++98, define bool, false, true as a GNU extension. */
#            define bool bool
#            define false false
#            define true true
#        endif /* __cplusplus < 201103L */
#    endif     /* __cplusplus */
#endif         /* NO_STDBOOL */

#endif /* AWS_COMMON_STDBOOL_H */
/* clang-format on */
