#ifndef AWS_COMMON_PREDICATES_H
#define AWS_COMMON_PREDICATES_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

/**
 * Returns whether all bytes of the two byte arrays match.
 */
#if defined(AWS_DEEP_CHECKS) && (AWS_DEEP_CHECKS == 1)
#    ifdef CBMC
/* clang-format off */
#        define AWS_BYTES_EQ(arr1, arr2, len)                                                                              \
            __CPROVER_forall {                                                                                             \
                int i;                                                                                                     \
                (i >= 0 && i < len) ==> ((const uint8_t *)&arr1)[i] == ((const uint8_t *)&arr2)[i]                         \
            }
/* clang-format on */
#    else
#        define AWS_BYTES_EQ(arr1, arr2, len) (memcmp(arr1, arr2, len) == 0)
#    endif /* CBMC */
#else
#    define AWS_BYTES_EQ(arr1, arr2, len) (1)
#endif /* (AWS_DEEP_CHECKS == 1) */

#endif /* AWS_COMMON_PREDICATES_H */
