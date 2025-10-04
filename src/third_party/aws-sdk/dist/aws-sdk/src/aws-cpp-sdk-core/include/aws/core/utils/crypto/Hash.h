/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/crypto/HashResult.h>
#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/memory/stl/AWSString.h>

namespace Aws
{
    namespace Utils
    {
        namespace Crypto
        {
            /**
             * Interface for computing hash codes using various hash algorithms
             */
            class AWS_CORE_API Hash
            {
            public:

                Hash() {}
                virtual ~Hash() {}

                /**
                * Calculates a Hash digest
                */
                virtual HashResult Calculate(const Aws::String& str) = 0;

                /**
                * Calculates a Hash digest on a stream (the entire stream is read)
                */
                virtual HashResult Calculate(Aws::IStream& stream) = 0;

                /**
                 * Updates a Hash digest
                 */
                virtual void Update(unsigned char*, size_t bufferSize) = 0;

                /**
                 * Get the result in the current value
                 */
                virtual HashResult GetHash() = 0;

                // when hashing streams, this is the size of our internal buffer we read the stream into
                static const uint32_t INTERNAL_HASH_STREAM_BUFFER_SIZE = 8192;
            };

            /**
             * Simple abstract factory interface. Subclass this and create a factory if you want to control
             * how Hash objects are created.
             */
            class AWS_CORE_API HashFactory
            {
            public:
                virtual ~HashFactory() {}

                /**
                 * Factory method. Returns hash implementation.
                 */
                virtual std::shared_ptr<Hash> CreateImplementation() const = 0;

                /**
                 * Opportunity to make any static initialization calls you need to make.
                 * Will only be called once.
                 */
                virtual void InitStaticState() {}

                /**
                 * Opportunity to make any static cleanup calls you need to make.
                 * will only be called at the end of the application.
                 */
                virtual void CleanupStaticState() {}
            };

        } // namespace Crypto
    } // namespace Utils
} // namespace Aws

