#pragma once
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/crt/StlAllocator.h>
#include <aws/crt/Types.h>

#include <aws/common/uuid.h>

namespace Aws
{
    namespace Crt
    {
        /**
         * Utility class for creating UUIDs and serializing them to a string
         */
        class AWS_CRT_CPP_API UUID final
        {
          public:
            UUID() noexcept;
            UUID(const String &str) noexcept;

            UUID &operator=(const String &str) noexcept;

            bool operator==(const UUID &other) noexcept;
            bool operator!=(const UUID &other) noexcept;
            operator String() const;
            operator ByteBuf() const noexcept;

            inline operator bool() const noexcept { return m_good; }

            int GetLastError() const noexcept;

            String ToString() const;

          private:
            aws_uuid m_uuid;
            bool m_good;
        };
    } // namespace Crt
} // namespace Aws
