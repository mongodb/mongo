/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Job API (private C++ API)
 */

#ifndef QPL_SOURCES_C_API_OTHER_OPERATIONS_CRC64_HPP_
#define QPL_SOURCES_C_API_OTHER_OPERATIONS_CRC64_HPP_

#include "qpl/c_api/defs.h"


/**
 * @anchor CRC_OPERATION
 * @brief Calculates CRC64 calculation
 *
 * @param [in,out] job_ptr pointer onto user specified @ref qpl_job
 *
 * @details For operation execution, you must set the following parameters in `qpl_job_ptr`:
 *          - @ref qpl_job.next_in_ptr            - start address of the input stream
 *          - @ref qpl_job.available_in           - number of bytes in the input stream
 *          - @ref qpl_job.crc64_poly             - polynomial for crc
 *
 * @note Function looks for values @ref QPL_FLAG_CRC64_BE, @ref QPL_FLAG_CRC64_INV.
 *
 * @note Operation result crc64 is available in the @ref qpl_job.crc64 field.
 *
 * @warning If crc64_poly is 0, it will be error (@ref QPL_STS_SIZE_ERR).
 *
 * @return
 *      - @ref QPL_STS_OK
 *      - @ref QPL_STS_NULL_PTR_ERR
 *      - @ref QPL_STS_CRC64_BAD_POLYNOM
 *
 * Example of main usage:
 * @snippet low-level-api/crc64_example.cpp QPL_LOW_LEVEL_CRC64_EXAMPLE
 *
 */
uint32_t perform_crc64(qpl_job *const job_ptr) noexcept;

#endif //QPL_SOURCES_C_API_OTHER_OPERATIONS_CRC64_HPP_
