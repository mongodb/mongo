/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/Array.h>

namespace Aws
{
    namespace Utils
    {
        static const size_t UUID_BINARY_SIZE = 0x10;

        /**
         * Class encapsulating a UUID. This is platform dependent. The method you are most likely interested in is RandomUUID().
         */
        class AWS_CORE_API UUID
        {
        public:
            /**
             * Parses a GUID string into the raw data.
             */
            UUID(const Aws::String&);
            /**
             * Sets the raw uuid data
             */
            UUID(const unsigned char uuid[UUID_BINARY_SIZE]);

            /**
             * Returns the current UUID as a GUID string
             */
            operator Aws::String() const;
            /**
             * Returns a copy of the raw uuid
             */
            inline operator ByteBuffer() const { return ByteBuffer(m_uuid, sizeof(m_uuid)); }

            /**
             * Generates a UUID. It will always try to prefer a random implementation from the entropy source on the machine. If none, is available, it will
             * fallback to the mac address and timestamp implementation.
             */
            static Aws::Utils::UUID RandomUUID();

            /**
             * Generates a pseudo-random UUID.
             */
            static Aws::Utils::UUID PseudoRandomUUID();

        private:
            unsigned char m_uuid[UUID_BINARY_SIZE];
        };
    }
}
