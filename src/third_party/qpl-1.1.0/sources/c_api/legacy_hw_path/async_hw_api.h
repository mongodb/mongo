/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef HW_PATH_ML_HW_API_H_
#define HW_PATH_ML_HW_API_H_

#include "hw_accelerator_api.h"

#ifdef __cplusplus
extern "C" {
#endif

QPL_API (qpl_status, hw_submit_job, (qpl_job * qpl_job_ptr));

QPL_API (qpl_status, hw_check_job, (qpl_job * qpl_job_ptr));

QPL_API (uint32_t,   hw_get_job_size, ());

#ifdef __cplusplus
}
#endif

#endif //HW_PATH_ML_HW_API_H_
