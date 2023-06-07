/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

#include "algorithm"

#if defined( linux )

#include <inttypes.h>

#include "hw_device.hpp"
#include "hw_descriptors_api.h"

#ifdef DYNAMIC_LOADING_LIBACCEL_CONFIG
#include "hw_configuration_driver.h"
#else //DYNAMIC_LOADING_LIBACCEL_CONFIG=OFF
#include "hw_devices.h"
#include "hw_definitions.h"
#include "libaccel_config.h"
#endif //DYNAMIC_LOADING_LIBACCEL_CONFIG

static const uint8_t  accelerator_name[]      = "iax";                         /**< Accelerator name */
static const uint32_t accelerator_name_length = sizeof(accelerator_name) - 2u; /**< Last symbol index */

static inline bool own_search_device_name(const uint8_t *src_ptr,
                                          const uint32_t name,
                                          const uint32_t name_size) noexcept {
    const uint8_t null_terminator = '\0';

    for (size_t symbol_idx = 0u; null_terminator != src_ptr[symbol_idx + name_size]; symbol_idx++) {
        const auto *candidate_ptr = reinterpret_cast<const uint32_t *>(src_ptr + symbol_idx);

        // Convert the first 3 bytes to lower case and make the 4th 0xff
        if (name == (*candidate_ptr | CHAR_MSK)) {
            return true;
        }
    }

    return false;
}

namespace qpl::ml::dispatcher {

void hw_device::fill_hw_context(hw_accelerator_context *const hw_context_ptr) const noexcept {
    // Restore device properties
    hw_context_ptr->device_properties.max_set_size                  = hw_device::get_max_set_size();
    hw_context_ptr->device_properties.max_decompressed_set_size     = hw_device::get_max_decompressed_set_size();
    hw_context_ptr->device_properties.indexing_support_enabled      = hw_device::get_indexing_support_enabled();
    hw_context_ptr->device_properties.decompression_support_enabled = hw_device::get_decompression_support_enabled();
    hw_context_ptr->device_properties.max_transfer_size             = hw_device::get_max_transfer_size();
    hw_context_ptr->device_properties.cache_flush_available         = hw_device::get_cache_flush_available();
    hw_context_ptr->device_properties.cache_write_available         = hw_device::get_cache_write_available();
    hw_context_ptr->device_properties.overlapping_available         = hw_device::get_overlapping_available();
    hw_context_ptr->device_properties.block_on_fault_enabled        = hw_device::get_block_on_fault_available();
}

auto hw_device::enqueue_descriptor(void *desc_ptr) const noexcept -> bool {
    uint8_t retry = 0u;
    static thread_local std::uint32_t wq_idx = 0;

    // For small low-latency cases WQ with small transfer size may be preferable
    // TODO: order WQs by priority and engines capacity, check transfer sizes and other possible features
    for (uint64_t try_count = 0u; try_count < queue_count_; ++try_count) {
        hw_iaa_descriptor_set_block_on_fault((hw_descriptor *) desc_ptr, working_queues_[wq_idx].get_block_on_fault());

        retry = working_queues_[wq_idx].enqueue_descriptor(desc_ptr);
        wq_idx = (wq_idx+1) % queue_count_;
        if (!retry) {
            break;
        }
    }

    return static_cast<bool>(retry);
}

auto hw_device::get_max_set_size() const noexcept -> uint32_t {
    return GC_MAX_SET_SIZE(gen_cap_register_);
}

auto hw_device::get_max_decompressed_set_size() const noexcept -> uint32_t {
    return GC_MAX_DECOMP_SET_SIZE(gen_cap_register_);
}

auto hw_device::get_indexing_support_enabled() const noexcept -> uint32_t {
    return GC_IDX_SUPPORT(gen_cap_register_);
}

auto hw_device::get_decompression_support_enabled() const noexcept -> bool {
    return GC_DECOMP_SUPPORT(gen_cap_register_);
}

auto hw_device::get_max_transfer_size() const noexcept -> uint32_t {
    return GC_MAX_TRANSFER_SIZE(gen_cap_register_);
}

auto hw_device::get_cache_flush_available() const noexcept -> bool {
    return GC_CACHE_FLUSH(gen_cap_register_);
}

auto hw_device::get_cache_write_available() const noexcept -> bool {
    return GC_CACHE_WRITE(gen_cap_register_);
}

auto hw_device::get_overlapping_available() const noexcept -> bool {
    return GC_OVERLAPPING(gen_cap_register_);
}

auto hw_device::get_block_on_fault_available() const noexcept -> bool {
    return GC_BLOCK_ON_FAULT(gen_cap_register_);
}

auto hw_device::initialize_new_device(descriptor_t *device_descriptor_ptr) noexcept -> hw_accelerator_status {
    // Device initialization stage
    auto       *device_ptr          = reinterpret_cast<accfg_device *>(device_descriptor_ptr);
    const auto *name_ptr            = reinterpret_cast<const uint8_t *>(accfg_device_get_devname(device_ptr));
    const bool  is_iaa_device       = own_search_device_name(name_ptr, IAA_DEVICE, accelerator_name_length);

    version_major_ = accfg_device_get_version(device_ptr)>>8u;
    version_minor_ = accfg_device_get_version(device_ptr)&0xFF;

    DIAG("%5s: ", name_ptr);
    if (!is_iaa_device) {
        DIAGA("UNSUPPORTED %5s\n", name_ptr);
        return HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE;
    }
    if (ACCFG_DEVICE_ENABLED != accfg_device_get_state(device_ptr)) {
        DIAGA("DISABLED %5s\n", name_ptr);
        return HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE;
    }
    DIAGA("\n");

    gen_cap_register_ = accfg_device_get_gen_cap(device_ptr);
    numa_node_id_     = accfg_device_get_numa_node(device_ptr);

    DIAG("%5s: version: %d.%d\n", name_ptr, version_major_, version_minor_);
    DIAG("%5s: numa: %" PRIu64 "\n", name_ptr, numa_node_id_);
    DIAG("%5s: GENCAP: %" PRIu64 "\n", name_ptr, gen_cap_register_);
    DIAG("%5s: GENCAP: block on fault support:              %d\n",          name_ptr, get_block_on_fault_available());
    DIAG("%5s: GENCAP: overlapping copy support:            %d\n",          name_ptr, get_overlapping_available());
    DIAG("%5s: GENCAP: cache control support (memory):      %d\n",          name_ptr, get_cache_write_available());
    DIAG("%5s: GENCAP: cache control support (cache flush): %d\n",          name_ptr, get_cache_flush_available());
    DIAG("%5s: GENCAP: maximum supported transfer size:     %" PRIu32 "\n", name_ptr, get_max_transfer_size());
    DIAG("%5s: GENCAP: decompression support:               %d\n",          name_ptr, get_decompression_support_enabled());
    DIAG("%5s: GENCAP: indexing support:                    %d\n",          name_ptr, get_indexing_support_enabled());
    DIAG("%5s: GENCAP: maximum decompression set size:      %d\n",          name_ptr, get_max_decompressed_set_size());
    DIAG("%5s: GENCAP: maximum set size:                    %d\n",          name_ptr, get_max_set_size());

    // Working queues initialization stage
    auto *wq_ptr = accfg_wq_get_first(device_ptr);
    auto wq_it   = working_queues_.begin();

    DIAG("%5s: getting device WQs\n", name_ptr);
    while (nullptr != wq_ptr) {
        if (HW_ACCELERATOR_STATUS_OK == wq_it->initialize_new_queue(wq_ptr)) {
            wq_it++;

            std::push_heap(working_queues_.begin(), wq_it,
                           [](const hw_queue &a, const hw_queue &b) -> bool {
                               return a.priority() < b.priority();
                           });
        }

        wq_ptr = accfg_wq_get_next(wq_ptr);
    }

    // Check number of working queues
    queue_count_ = std::distance(working_queues_.begin(), wq_it);

    if (queue_count_ > 1) {
        auto begin = working_queues_.begin();
        auto end   = begin + queue_count_;

        std::sort_heap(begin, end, [](const hw_queue &a, const hw_queue &b) -> bool {
            return a.priority() < b.priority();
        });
    }

    if (queue_count_ == 0) {
        return HW_ACCELERATOR_WORK_QUEUES_NOT_AVAILABLE;
    }

    return HW_ACCELERATOR_STATUS_OK;
}

auto hw_device::size() const noexcept -> size_t {
    return queue_count_;
}

auto hw_device::numa_id() const noexcept -> uint64_t {
    return numa_node_id_;
}

auto hw_device::begin() const noexcept -> queues_container_t::const_iterator {
    return working_queues_.cbegin();
}

auto hw_device::end() const noexcept -> queues_container_t::const_iterator {
    return working_queues_.cbegin() + queue_count_;
}

}

#endif //linux
