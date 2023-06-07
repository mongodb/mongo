/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle Layer API (private C++ API)
 */

#include "hw_dispatcher.hpp"
#include <mutex>

#if defined( linux )

#endif

#define QPL_HWSTS_RET(expr, err_code) { if( expr ) { return( err_code ); }}

namespace qpl::ml::dispatcher {

hw_dispatcher::hw_dispatcher() noexcept {
    hw_init_status_ = hw_dispatcher::initialize_hw();
    hw_support_     = hw_init_status_ == HW_ACCELERATOR_STATUS_OK;
}

auto hw_dispatcher::initialize_hw() noexcept -> hw_accelerator_status {
#if defined( linux )
    accfg_ctx *ctx_ptr = nullptr;

    DIAG("Intel QPL version %s\n", QPL_VERSION);

#ifdef DYNAMIC_LOADING_LIBACCEL_CONFIG
    hw_accelerator_status status = hw_initialize_accelerator_driver(&hw_driver_);
    QPL_HWSTS_RET(status != HW_ACCELERATOR_STATUS_OK, status);
#endif


    DIAG("creating context\n");
    int32_t context_creation_status = accfg_new(&ctx_ptr);
    QPL_HWSTS_RET(0u != context_creation_status, HW_ACCELERATOR_LIBACCEL_ERROR);

    // Retrieve first device in the system given the passed in context
    DIAG("enumerating devices\n");
    auto *dev_tmp_ptr = accfg_device_get_first(ctx_ptr);
    auto device_it    = devices_.begin();

    while (nullptr != dev_tmp_ptr) {
        if (HW_ACCELERATOR_STATUS_OK == device_it->initialize_new_device(dev_tmp_ptr)) {
            device_it++;
        }

        // Retrieve the "next" device in the system based on given device
        dev_tmp_ptr = accfg_device_get_next(dev_tmp_ptr);
    }

    device_count_ = std::distance(devices_.begin(), device_it);

    if (device_count_ <= 0) {
        return HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE; // No devices -> No WQ
    }

    hw_context_.set_driver_context_ptr(ctx_ptr);

    return HW_ACCELERATOR_STATUS_OK;
#else
    // Windows is not supported
    return HW_ACCELERATOR_LIBACCEL_NOT_FOUND;
#endif
}

hw_dispatcher::~hw_dispatcher() noexcept {
#if defined( linux )
    // Variables
    auto *context_ptr = hw_context_.get_driver_context_ptr();

    if (context_ptr != nullptr) {
        accfg_unref(context_ptr);
    }

#ifdef DYNAMIC_LOADING_LIBACCEL_CONFIG
    hw_finalize_accelerator_driver(&hw_driver_);
#endif

    // Zeroing values
    hw_context_.set_driver_context_ptr(nullptr);
#endif
}

// starting from C++11 standard,
// it is guarantued that the following would be thread-safe
// and created only once
// (case: static variables with block scope)
auto hw_dispatcher::get_instance() noexcept -> hw_dispatcher & {
    static hw_dispatcher instance{};
    return instance;
}

void hw_dispatcher::fill_hw_context(hw_accelerator_context *const hw_context_ptr) noexcept {
#if defined( linux )
    // Restore context
    hw_context_ptr->ctx_ptr = hw_context_.get_driver_context_ptr();

    // Restore device properties
    // We take the first one as all configurations across the platform should be the same for all devices
    devices_[0].fill_hw_context(hw_context_ptr);
#endif
}

auto hw_dispatcher::get_hw_init_status() const noexcept -> hw_accelerator_status {
    return hw_init_status_;
}

auto hw_dispatcher::is_hw_support() const noexcept -> bool {
    return hw_support_;
}

#if defined( linux )

auto hw_dispatcher::begin() const noexcept -> device_container_t::const_iterator {
    return devices_.cbegin();
}

auto hw_dispatcher::end() const noexcept -> device_container_t::const_iterator {
    return devices_.cbegin() + device_count_;
}

auto hw_dispatcher::device_count() const noexcept -> size_t {
    return device_count_;
}

auto hw_dispatcher::device(size_t idx) const noexcept -> const hw_device & {
    return devices_[idx % device_count_];
}

void hw_dispatcher::hw_context::set_driver_context_ptr(accfg_ctx *driver_context_ptr) noexcept {
    driver_context_ptr_ = driver_context_ptr;
}

[[nodiscard]] auto hw_dispatcher::hw_context::get_driver_context_ptr() noexcept -> accfg_ctx * {
    return driver_context_ptr_;
}

#endif
}
