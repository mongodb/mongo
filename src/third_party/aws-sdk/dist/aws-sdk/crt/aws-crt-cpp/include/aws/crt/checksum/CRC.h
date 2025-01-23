#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/Exports.h>
#include <aws/crt/Types.h>

namespace Aws
{
    namespace Crt
    {
        namespace Checksum
        {
            /**
             * The entry point function to perform a CRC32 (Ethernet, gzip) computation.
             * Selects a suitable implementation based on hardware capabilities.
             * Pass previousCRC32 if updating a running checksum.
             */
            uint32_t AWS_CRT_CPP_API ComputeCRC32(ByteCursor input, uint32_t previousCRC32 = 0) noexcept;

            /**
             * The entry point function to perform a Castagnoli CRC32c (iSCSI) computation.
             * Selects a suitable implementation based on hardware capabilities.
             * Pass previousCRC32C if updating a running checksum.
             */
            uint32_t AWS_CRT_CPP_API ComputeCRC32C(ByteCursor input, uint32_t previousCRC32C = 0) noexcept;

            /**
             * The entry point function to perform a CRC64-NVME (a.k.a. CRC64-Rocksoft) computation.
             * Selects a suitable implementation based on hardware capabilities.
             * Pass previousCRC64NVME if updating a running checksum.
             * There are many variants of CRC64 algorithms. This CRC64 variant is bit-reflected (based on
             * the non bit-reflected polynomial 0xad93d23594c93659) and inverts the CRC input and output bits.
             */
            uint64_t AWS_CRT_CPP_API ComputeCRC64NVME(ByteCursor input, uint64_t previousCRC64NVME = 0) noexcept;
        } // namespace Checksum
    } // namespace Crt
} // namespace Aws
