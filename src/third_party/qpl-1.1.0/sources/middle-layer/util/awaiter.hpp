/*******************************************************************************
 * Copyright (C) 2022 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 ******************************************************************************/

/*
 *  Intel® Query Processing Library (Intel® QPL)
 *  Middle Layer API (private C++ API)
 */

#ifndef QPL_AWAITER_HPP
#define QPL_AWAITER_HPP


#include <cstdint>

namespace qpl::ml {

/**
 * @brief Class that allows to defer scope exit to the moment when a certain address is changed
 */
class awaiter final {
public:

    /**
     * @brief Constructor of the class
     *
     * @param address       pointer to memory that should be asynchronously changed
     * @param initial_value value to compare with
     * @param period        number of clocks between checks
     */
    explicit awaiter(volatile void *address,
                     uint8_t initial_value,
                     uint32_t period = 200) noexcept;

    /**
     * @brief Destructor that performs actual wait
     */
    ~awaiter() noexcept;

    static void wait_for(volatile void *address, uint8_t initial_value) noexcept;

private:
    volatile uint8_t *address_ptr_  = nullptr;  /**< Pointer to memory that should be asynchronously changed */
    uint32_t         period_        = 0u;       /**< Number of clocks between checks */
    uint8_t          initial_value_ = 0u;       /**< Value to compare with */
    uint32_t         idle_state_    = 0u;       /**< State for CPU wait control */
};

}

#endif //QPL_AWAITER_HPP
