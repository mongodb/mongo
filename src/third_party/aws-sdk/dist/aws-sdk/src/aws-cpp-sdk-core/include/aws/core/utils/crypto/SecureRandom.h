/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once
#include <type_traits>
#include <memory>
#include <cassert>

namespace Aws
{
    namespace Utils
    {
        namespace Crypto
        {
            /**
             * Interface for generating Random Bytes with guaranteed entropy for use with cryptographic functions.
             * An instance is not guaranteed to be thread safe. This is intentional, that is needless overhead to 
             *  pay for something you probably don't need. If you encounter a need for thread safety, you are responsible
             *  for memory fencing.
             */
            class SecureRandomBytes
            {
            public:
                SecureRandomBytes() : m_failure(false)
                {
                }

                virtual ~SecureRandomBytes() = default;

                /**
                 * fill in buffer of size bufferSize with random bytes
                 */
                virtual void GetBytes(unsigned char* buffer, size_t bufferSize) = 0;

                /**
                 * Always check this. If anything goes wrong, this tells you
                 */
                operator bool() const { return !m_failure; }

            protected:
                bool m_failure;
            };

            /**
             * Random Number generator for integral types. Guaranteed to have entropy or your program will crash.
             */
            template <typename DataType = uint64_t>
            class SecureRandom
            {
            public:
                /**
                 * Initialize with the results of CreateSecureRandomBytesImplementation().
                 *  An instance is not guaranteed to be thread safe. This is intentional, that is needless overhead to 
                 *  pay for something you probably don't need. If you encounter a need for thread safety, you are responsible
                 *  for memory fencing.
                 */
                SecureRandom(const std::shared_ptr<SecureRandomBytes>& entropySource) : m_entropy(entropySource)
                    { static_assert(std::is_unsigned<DataType>::value, "Type DataType must be integral"); }

                virtual ~SecureRandom() = default;

                virtual void Reset() {}

                /**
                 * Generate a random number of DataType
                 */
                virtual DataType operator()()
                {
                    DataType value(0);
                    unsigned char buffer[sizeof(DataType)];
                    m_entropy->GetBytes(buffer, sizeof(DataType));

                    assert(*m_entropy);
                    if(*m_entropy)
                    {
                        for (size_t i = 0; i < sizeof(DataType); ++i)
                        {
                             value <<= 8;
                             value |= buffer[i];

                        }
                    }

                    return value;
                }

                operator bool() const { return *m_entropy; }

            private:
                std::shared_ptr<SecureRandomBytes> m_entropy;
            };           

            class SecureRandomFactory
            {
            public:
                virtual ~SecureRandomFactory() = default;

                /**
                 * Factory method. Returns SecureRandom implementation.
                 */
                virtual std::shared_ptr<SecureRandomBytes> CreateImplementation() const = 0;

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
        }
    }
}
