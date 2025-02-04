/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
    namespace Http
    {
        /**
         * Enum representing URI scheme.
         */
        enum class Scheme
        {
            HTTP,
            HTTPS
        };

        namespace SchemeMapper
        {
            /**
             * Converts a Scheme instance to a String.
             */
            AWS_CORE_API const char* ToString(Scheme scheme);
            /**
            * Converts a string instance to a Scheme. Defaults to https.
            */
            AWS_CORE_API Scheme FromString(const char* name);
        } // namespace SchemeMapper
    } // namespace Http
} // namespace Aws

