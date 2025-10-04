/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/utils/memory/stl/AWSMap.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/Core_EXPORTS.h>
#include <aws/core/utils/threading/ReaderWriterLock.h>

namespace Aws
{
    namespace Utils
    {
        /**
         * This container is for storing unknown enum values that are encountered during parsing.
         * This is to work around the round-tripping enums problem. It's really just a simple thread-safe
         * hashmap.
         */
        class AWS_CORE_API EnumParseOverflowContainer
        {
        public:
            const Aws::String& RetrieveOverflow(int hashCode) const;
            void StoreOverflow(int hashCode, const Aws::String& value);

        private:
            mutable Aws::Utils::Threading::ReaderWriterLock m_overflowLock;
            Aws::Map<int, Aws::String> m_overflowMap;
            Aws::String m_emptyString;
        };
    }
}


