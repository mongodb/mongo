/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/utils/Outcome.h>

namespace Aws
{
    namespace Utils
    {

        /**
         * Template class representing the std::future object of outcome of calling some other API.
         * It will contain a future of an either a successful result or the failure error.
         * The caller must check whether the outcome of the request was a success before attempting to access
         *  the result or the error.
         */
        template<typename R, typename E> // Result, Error
        using FutureOutcome = Aws::Utils::Outcome<R, E>;
    } // namespace Utils
} // namespace Aws
