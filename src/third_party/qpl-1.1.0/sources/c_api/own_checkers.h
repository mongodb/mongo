/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_SOURCES_INCLUDE_OWN_QPL_CHECKERS_H_
#define QPL_SOURCES_INCLUDE_OWN_QPL_CHECKERS_H_

#include "qpl/c_api/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define QPL_ERROR_RET(err_code) return( err_code )

#ifdef QPL_BADARG_CHECK
#define QPL_BADARG_RET(expr, err_code) { if( expr ) { QPL_ERROR_RET( err_code ); }}
#else
#define QPL_BADARG_RET(expr, err_code)
#endif


#define OWN_RETURN_ERROR(expression, error_code) { if (expression) { return (error_code); }}

#define QPL_BAD_PTR_RET(ptr) QPL_BADARG_RET(NULL==(ptr), QPL_STS_NULL_PTR_ERR)

#define HW_IMMEDIATELY_RET(expression, status) \
if((expression)) { \
    return (qpl_status) (status); \
}

#define HW_IMMEDIATELY_RET_NULLPTR(expression) \
if(NULL == (expression)) { \
    return QPL_STS_NULL_PTR_ERR; \
}

#define QPL_BAD_PTR2_RET(ptr1, ptr2)\
    {QPL_BAD_PTR_RET(ptr1); QPL_BAD_PTR_RET(ptr2)}

#define QPL_BAD_SIZE_RET(n)\
    QPL_BADARG_RET((n)<=0, QPL_STS_SIZE_ERR)

#define QPL_VALID_OP (\
    (1ULL << qpl_op_decompress    ) |\
    (1ULL << qpl_op_compress      ) |\
    (1ULL << qpl_op_crc64         ) |\
    (1ULL << qpl_op_extract       ) |\
    (1ULL << qpl_op_select        ) |\
    (1ULL << qpl_op_expand        ) |\
    (1ULL << qpl_op_scan_eq       ) |\
    (1ULL << qpl_op_scan_ne       ) |\
    (1ULL << qpl_op_scan_lt       ) |\
    (1ULL << qpl_op_scan_le       ) |\
    (1ULL << qpl_op_scan_gt       ) |\
    (1ULL << qpl_op_scan_ge       ) |\
    (1ULL << qpl_op_scan_range    ) |\
    (1ULL << qpl_op_scan_not_range))

#define QPL_BAD_OP_RET(op)\
   { QPL_BADARG_RET((0 == (((uint64_t)QPL_VALID_OP >> op) & 1)), QPL_STS_OPERATION_ERR)};

#ifdef __cplusplus
}
#endif

#endif //QPL_SOURCES_INCLUDE_OWN_QPL_CHECKERS_H_
