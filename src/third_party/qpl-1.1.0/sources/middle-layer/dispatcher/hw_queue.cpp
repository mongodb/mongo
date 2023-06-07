/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#if defined( linux )

#include <fcntl.h>
#include <sys/mman.h>

#include "hw_queue.hpp"

#ifdef DYNAMIC_LOADING_LIBACCEL_CONFIG
#include "hw_configuration_driver.h"
#else //DYNAMIC_LOADING_LIBACCEL_CONFIG=OFF
#include "hw_devices.h"
#include "hw_definitions.h"
#include "libaccel_config.h"
#endif //DYNAMIC_LOADING_LIBACCEL_CONFIG

#define QPL_HWSTS_RET(expr, err_code) { if( expr ) { return( err_code ); }}
#define DEC_BASE 10u         /**< @todo */
#define DEC_CHAR_BASE ('0')  /**< @todo */
#define DEC_MAX_INT_BUF 16u  /**< @todo */

namespace qpl::ml::dispatcher {

hw_queue::hw_queue(hw_queue &&other) noexcept {
    priority_      = other.priority_;
    portal_mask_   = other.portal_mask_;
    portal_ptr_    = other.portal_ptr_;
    portal_offset_ = 0;

    other.portal_ptr_ = nullptr;
}

auto hw_queue::operator=(hw_queue &&other) noexcept -> hw_queue & {
    priority_      = other.priority_;
    portal_mask_   = other.portal_mask_;
    portal_ptr_    = other.portal_ptr_;
    portal_offset_ = 0;

    other.portal_ptr_ = nullptr;

    return *this;
}

hw_queue::~hw_queue() noexcept {
    // Freeing resources
    if (portal_ptr_ != nullptr) {
        munmap(portal_ptr_, 0x1000u);

        portal_ptr_ = nullptr;
    }
}

void hw_queue::set_portal_ptr(void *value_ptr) noexcept {
    portal_offset_ = reinterpret_cast<uint64_t>(value_ptr) & OWN_PAGE_MASK;
    portal_mask_   = reinterpret_cast<uint64_t>(value_ptr) & (~OWN_PAGE_MASK);
    portal_ptr_    = value_ptr;
}

auto hw_queue::get_portal_ptr() const noexcept -> void * {
    uint64_t offset = portal_offset_++;
    offset = (offset << 6) & OWN_PAGE_MASK;
    return reinterpret_cast<void *>(offset | portal_mask_);
}

auto hw_queue::enqueue_descriptor(void *desc_ptr) const noexcept -> qpl_status {
    uint8_t retry = 0u;

    void *current_place_ptr = get_portal_ptr();
    asm volatile("sfence\t\n"
                 ".byte 0xf2, 0x0f, 0x38, 0xf8, 0x02\t\n"
                 "setz %0\t\n"
    : "=r"(retry) : "a" (current_place_ptr), "d" (desc_ptr));

    return static_cast<qpl_status>(retry);
}

auto hw_queue::initialize_new_queue(void *wq_descriptor_ptr) noexcept -> hw_accelerator_status {

    auto *work_queue_ptr        = reinterpret_cast<accfg_wq *>(wq_descriptor_ptr);
    char path[64];
#ifdef LOG_HW_INIT
    auto work_queue_dev_name    = accfg_wq_get_devname(work_queue_ptr);
#endif

    if (ACCFG_WQ_ENABLED != accfg_wq_get_state(work_queue_ptr)) {
        DIAG("     %7s: DISABLED\n", work_queue_dev_name);
        return HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE;
    }

    if (ACCFG_WQ_SHARED != accfg_wq_get_mode(work_queue_ptr)) {
        DIAG("     %7s: UNSUPPOTED\n", work_queue_dev_name);
        return HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE;
    }

    DIAG("     %7s:\n", work_queue_dev_name);
    auto status = accfg_wq_get_user_dev_path(work_queue_ptr, path, 64 - 1);
    QPL_HWSTS_RET((0 > status), HW_ACCELERATOR_LIBACCEL_ERROR);

    DIAG("     %7s: opening descriptor %s", work_queue_dev_name, path);
    auto fd = open(path, O_RDWR);
    if(0 >= fd)
    {
        DIAGA(", access denied\n");
        return HW_ACCELERATOR_LIBACCEL_ERROR;
    }

    auto *region_ptr = mmap(nullptr, 0x1000u, PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
    close(fd);
    if(MAP_FAILED == region_ptr)
    {
        DIAGA(", limited MSI-X mapping failed\n");
        return HW_ACCELERATOR_LIBACCEL_ERROR;
    }
    DIAGA("\n");

    priority_       = accfg_wq_get_priority(work_queue_ptr);
    block_on_fault_ = accfg_wq_get_block_on_fault(work_queue_ptr);

#if 0
    DIAG("     %7s: size:        %d\n", work_queue_dev_name, accfg_wq_get_size(work_queue_ptr));
    DIAG("     %7s: threshold:   %d\n", work_queue_dev_name, accfg_wq_get_threshold(work_queue_ptr));
    DIAG("     %7s: priority:    %d\n", work_queue_dev_name, priority_);
    DIAG("     %7s: group:       %d\n", work_queue_dev_name, group_id);

    for(struct accfg_engine *engine = accfg_engine_get_first(device_ptr);
            engine != NULL; engine = accfg_engine_get_next(engine))
    {
        if(accfg_engine_get_group_id(engine) == group_id)
            DIAG("            %s\n", accfg_engine_get_devname(engine));
    }
#else
    DIAG("     %7s: priority:    %d\n", work_queue_dev_name, priority_);
    DIAG("     %7s: bof:         %d\n", work_queue_dev_name, block_on_fault_);
#endif

    hw_queue::set_portal_ptr(region_ptr);

    return HW_ACCELERATOR_STATUS_OK;
}

auto hw_queue::priority() const noexcept -> int32_t {
    return priority_;
}

auto hw_queue::get_block_on_fault() const noexcept -> bool {
    return block_on_fault_;
}

}
#endif //linux
