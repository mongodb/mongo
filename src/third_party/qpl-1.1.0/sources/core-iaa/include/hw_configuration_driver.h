/*******************************************************************************
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#ifndef QPL_SOURCES_HW_PATH_INCLUDE_HW_CONFIGURATION_DRIVER_H_
#define QPL_SOURCES_HW_PATH_INCLUDE_HW_CONFIGURATION_DRIVER_H_

#if defined ( DYNAMIC_LOADING_LIBACCEL_CONFIG )

#include "hw_definitions.h"
#include "hw_devices.h"

#if defined( linux )

#include "libaccel_config.h"

#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Type of function that should be loaded from accelerator configuration driver
 */
typedef void *library_function;

/**
 * @brief Structure that maps function implementation to its name
 */
typedef struct {
    library_function function;          /**< Function address */
    const char       *function_name;    /**< Function name */
} qpl_desc_t;

/**
 * @brief Structure represents configuration driver used for access to accelerator instances and their properties
 */
typedef struct {
    void *driver_instance_ptr; /**< Pointer to a loaded driver */
} hw_driver_t;

/**
 * @brief Initializes driver functions
 *
 * @note Should be called only once
 *
 * @return status of initialization
 */
HW_PATH_GENERAL_API(hw_accelerator_status, initialize_accelerator_driver, (hw_driver_t *driver_ptr));

HW_PATH_GENERAL_API(void, finalize_accelerator_driver, (hw_driver_t *driver_ptr));

typedef int                     (*accfg_new_ptr)(accfg_ctx **ctx);

typedef accfg_dev *             (*accfg_device_get_first_ptr)(accfg_ctx *ctx);

typedef const char *            (*accfg_device_get_devname_ptr)(accfg_dev *device);

typedef accfg_dev *             (*accfg_device_get_next_ptr)(accfg_dev *device);

typedef accfg_wq *              (*accfg_wq_get_first_ptr)(accfg_dev *device);

typedef accfg_wq *              (*accfg_wq_get_next_ptr)(accfg_wq *wq);

typedef enum accfg_wq_state     (*accfg_wq_get_state_ptr)(accfg_wq *wq);

typedef int                     (*accfg_wq_get_id_ptr)(accfg_wq *wq);

typedef enum accfg_device_state (*accfg_device_get_state_ptr)(accfg_dev *device);

typedef accfg_ctx *             (*accfg_unref_ptr)(accfg_ctx *ctx);

typedef enum accfg_wq_mode      (*accfg_wq_get_mode_ptr)(accfg_wq *wq);

typedef unsigned long           (*accfg_device_get_gen_cap_ptr)(accfg_dev *device);

typedef int                     (*accfg_wq_get_user_dev_path_ptr)(accfg_wq *wq, char *buf, size_t size);

typedef int                     (*accfg_device_get_numa_node_ptr)(accfg_dev *device);

typedef int                     (*accfg_wq_get_priority_ptr)(accfg_wq *wq);

typedef const char *            (*accfg_wq_get_devname_ptr)(accfg_wq *wq);

typedef unsigned int            (*accfg_device_get_version_ptr)(accfg_dev *device);

typedef int                     (*accfg_wq_get_block_on_fault_ptr)(accfg_wq *wq);

#ifdef __cplusplus
}
#endif

#endif //if defined ( DYNAMIC_LOADING_LIBACCEL_CONFIG )
#endif //QPL_SOURCES_HW_PATH_INCLUDE_HW_CONFIGURATION_DRIVER_H_
