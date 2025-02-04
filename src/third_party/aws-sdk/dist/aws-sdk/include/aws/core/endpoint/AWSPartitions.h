/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once
#include <cstddef>
#include <aws/core/Core_EXPORTS.h>

namespace Aws
{
namespace Endpoint
{
    struct AWS_CORE_API AWSPartitions
    {
    public:
        static const size_t PartitionsBlobStrLen;
        static const size_t PartitionsBlobSize;

        static const char* GetPartitionsBlob();
    };
} // namespace Endpoint
} // namespace Aws
