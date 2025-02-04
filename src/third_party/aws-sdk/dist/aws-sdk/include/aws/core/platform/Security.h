/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>

#include <stddef.h>

namespace Aws
{
namespace Security
{

    /*
    * Securely clears a block of memory
    */
    AWS_CORE_API void SecureMemClear(unsigned char *data, size_t length);

} // namespace Security
} // namespace Aws

