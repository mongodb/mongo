/*******************************************************************************
 * Copyright (C) 2023 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#if defined( linux ) && defined ( DYNAMIC_LOADING_LIBACCEL_CONFIG )

#include "hw_configuration_driver.h"
#include "own_hw_definitions.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <dlfcn.h>

static const char *accelerator_configuration_driver_name = "libaccel-config.so.1";

/**
 * @brief Table with functions required from accelerator configuration library
 */
static qpl_desc_t functions_table[] = {
        {NULL, "accfg_new"},
        {NULL, "accfg_device_get_first"},
        {NULL, "accfg_device_get_devname"},
        {NULL, "accfg_device_get_next"},
        {NULL, "accfg_wq_get_first"},
        {NULL, "accfg_wq_get_next"},
        {NULL, "accfg_wq_get_state"},
        {NULL, "accfg_wq_get_mode"},
        {NULL, "accfg_wq_get_id"},
        {NULL, "accfg_device_get_state"},
        {NULL, "accfg_unref"},
        {NULL, "accfg_device_get_gen_cap"},
        {NULL, "accfg_device_get_numa_node"},
        {NULL, "accfg_wq_get_priority"},
        {NULL, "accfg_wq_get_user_dev_path"},
        {NULL, "accfg_wq_get_devname"},
        {NULL, "accfg_device_get_version"},
        {NULL, "accfg_wq_get_block_on_fault"},

        // Terminate list/init
        {NULL, NULL}
};


static inline hw_accelerator_status own_load_accelerator_configuration_driver(void **driver_instance_pptr);

static inline bool own_load_configuration_functions(void *driver_instance_ptr);

hw_accelerator_status hw_initialize_accelerator_driver(hw_driver_t *driver_ptr) {

    // Variables
    driver_ptr->driver_instance_ptr = NULL;

    // Load DLL
    hw_accelerator_status status = own_load_accelerator_configuration_driver(&driver_ptr->driver_instance_ptr);

    if(status || driver_ptr->driver_instance_ptr == NULL) {
        hw_finalize_accelerator_driver(driver_ptr);

        return HW_ACCELERATOR_LIBACCEL_NOT_FOUND;
    }

    // If DLL is loaded successfully
    if (!own_load_configuration_functions(driver_ptr->driver_instance_ptr)) {
        hw_finalize_accelerator_driver(driver_ptr);

        return HW_ACCELERATOR_LIBACCEL_NOT_FOUND;
    }

    return HW_ACCELERATOR_STATUS_OK;
}

void hw_finalize_accelerator_driver(hw_driver_t *driver_ptr) {
    if (driver_ptr->driver_instance_ptr) {
        dlclose(driver_ptr->driver_instance_ptr);
    }

    driver_ptr->driver_instance_ptr = NULL;
}

int32_t accfg_new(accfg_ctx **ctx) {
    return ((accfg_new_ptr) functions_table[0].function)(ctx);
}

accfg_dev *accfg_device_get_first(accfg_ctx *ctx) {
    return ((accfg_device_get_first_ptr) functions_table[1].function)(ctx);
}

const char *accfg_device_get_devname(accfg_dev *device) {
    return ((accfg_device_get_devname_ptr) functions_table[2].function)(device);
}

accfg_dev *accfg_device_get_next(accfg_dev *device) {
    return ((accfg_device_get_next_ptr) functions_table[3].function)(device);
}

accfg_wq *accfg_wq_get_first(accfg_dev *device) {
    return ((accfg_wq_get_first_ptr) functions_table[4].function)(device);
}

accfg_wq *accfg_wq_get_next(accfg_wq *wq) {
    return ((accfg_wq_get_next_ptr) functions_table[5].function)(wq);
}

enum accfg_wq_state accfg_wq_get_state(accfg_wq *wq) {
    return ((accfg_wq_get_state_ptr) functions_table[6].function)(wq);
}

enum accfg_wq_mode accfg_wq_get_mode(accfg_wq *wq) {
    return ((accfg_wq_get_mode_ptr) functions_table[7].function)(wq);
}

int32_t accfg_wq_get_id(accfg_wq *wq) {
    return ((accfg_wq_get_id_ptr) functions_table[8].function)(wq);
}

enum accfg_device_state accfg_device_get_state(accfg_dev *device) {
    return ((accfg_device_get_state_ptr) functions_table[9].function)(device);
}

accfg_ctx *accfg_unref(accfg_ctx *ctx) {
    return ((accfg_unref_ptr) functions_table[10].function)(ctx);
}

uint64_t accfg_device_get_gen_cap(accfg_dev *device) {
    return ((accfg_device_get_gen_cap_ptr) functions_table[11].function)(device);
}

int accfg_device_get_numa_node(accfg_dev *device) {
    return ((accfg_device_get_numa_node_ptr) functions_table[12].function)(device);
}

int32_t accfg_wq_get_priority(accfg_wq *wq) {
    return ((accfg_wq_get_priority_ptr) functions_table[13].function)(wq);
}

int accfg_wq_get_user_dev_path(accfg_wq *wq, char *buf, size_t size) {
    return ((accfg_wq_get_user_dev_path_ptr) functions_table[14].function)(wq, buf, size);
}

const char * accfg_wq_get_devname(accfg_wq *wq) {
    return ((accfg_wq_get_devname_ptr) functions_table[15].function)(wq);
}

unsigned int accfg_device_get_version(accfg_dev *device) {
    return ((accfg_device_get_version_ptr) functions_table[16].function)(device);
}

int accfg_wq_get_block_on_fault(accfg_wq *wq) {
    return ((accfg_wq_get_block_on_fault_ptr) functions_table[17].function)(wq);
}

/* ------ Internal functions implementation ------ */

bool own_load_configuration_functions(void *driver_instance_ptr) {
    uint32_t i = 0u;

    DIAG("loading functions table:\n");
    while (functions_table[i].function_name) {
        DIAG("    loading %s\n", functions_table[i].function_name);
        functions_table[i].function = (library_function) dlsym(driver_instance_ptr, functions_table[i].function_name);
        i++;

        char *err_message = dlerror();

        if (err_message) {
            return false;
        }
    }

    return true;
}

hw_accelerator_status own_load_accelerator_configuration_driver(void **driver_instance_pptr) {

    DIAG("loading driver: %s\n", accelerator_configuration_driver_name);
    // Try to load the accelerator configuration library
    void *driver_instance_ptr = dlopen(accelerator_configuration_driver_name, RTLD_LAZY);

    if (!driver_instance_ptr) {
        // This is needed for error handle. We need to call dlerror
        // for emptying error message. Otherwise we will receive error
        // message during loading symbols from another library
        dlerror();

        return HW_ACCELERATOR_LIBACCEL_NOT_FOUND;
    }

    *driver_instance_pptr = driver_instance_ptr;

    return HW_ACCELERATOR_STATUS_OK;
}
#endif //if defined( linux ) && defined ( DYNAMIC_LOADING_LIBACCEL_CONFIG )
