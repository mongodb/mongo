/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/**
 * @date 3/23/2020
 * @brief Internal HW API functions for @ref hw_accelerator_get_context and @ref hw_accelerator_finalize API implementation
 *
 * @defgroup HW_ACCELERATOR_INIT_API Initialization API
 * @ingroup HW_PRIVATE_API
 * @{
 */

#if defined( linux )

#include "libaccel_config.h"

#endif

#include "hw_accelerator_api.h"
#include "dispatcher/hw_dispatcher.hpp"

extern "C" hw_accelerator_status hw_accelerator_get_context(hw_accelerator_context *const accel_context_ptr) {
    static auto &dispatcher = qpl::ml::dispatcher::hw_dispatcher::get_instance();
    if (!accel_context_ptr)
        return HW_ACCELERATOR_NULL_PTR_ERR;

    if (dispatcher.is_hw_support()) {
        dispatcher.fill_hw_context(accel_context_ptr);

        return HW_ACCELERATOR_STATUS_OK;
    }

    return dispatcher.get_hw_init_status();
}

extern "C" hw_accelerator_status hw_accelerator_finalize(hw_accelerator_context *const UNREFERENCED_PARAMETER(accel_context_ptr)) {
    return HW_ACCELERATOR_STATUS_OK;
}
