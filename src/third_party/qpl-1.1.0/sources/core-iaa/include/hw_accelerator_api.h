/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Hardware Interconnect API (private C API)
 */

/**
 * @brief Contains API to communicate with a SoC Accelerators.
 *
 * @defgroup HW_ACCELERATOR_API Accelerator API
 * @ingroup HW_PUBLIC_API
 * @{
 */

#ifndef HW_PATH_ACCELERATOR_API_H_
#define HW_PATH_ACCELERATOR_API_H_

#include "hw_definitions.h"
#include "hw_devices.h"
#include "hw_descriptors_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Helps to get structure that contains information about accelerator configuration
 *
 * @param [out] accel_context_ptr pointer to @ref hw_accelerator_context structure
 *
 * @return @ref hw_accelerator_status
 */
HW_PATH_GENERAL_API(hw_accelerator_status, accelerator_get_context, (hw_accelerator_context *const accel_context_ptr));


/**
 * @brief Finalize work with accelerator
 *
 * @param[in] accel_context_ptr pointer to @ref hw_accelerator_context structure
 *
 * @return @ref hw_accelerator_status
 */
HW_PATH_GENERAL_API(hw_accelerator_status, accelerator_finalize, (hw_accelerator_context *const accel_context_ptr));


/**
 * @todo API will be described after refactoring completed
 */
HW_PATH_GENERAL_API(hw_accelerator_status, accelerator_submit_descriptor, (hw_accelerator_context *const accel_context_ptr,
                                                                          const hw_descriptor *const descriptor_ptr,
                                                                          const uint64_t submit_flags));

/**
 * @brief hw_enqueue_descriptor - submits descriptor to corresponding portal via enqcmd
 *
 * @param[in]   desc_ptr        - pointer to descriptor
 * @param[in]   device_numa_id  - preferred NUMA node ID (-1 for automatic choice)
 *
 * @return 0 in case of success execution, or non-zero value, otherwise
 *
 */
hw_accelerator_status hw_enqueue_descriptor(void *desc_ptr, int32_t device_numa_id);
#ifdef __cplusplus
}
#endif

#endif // HW_PATH_ACCELERATOR_API_H_

/** @} */
