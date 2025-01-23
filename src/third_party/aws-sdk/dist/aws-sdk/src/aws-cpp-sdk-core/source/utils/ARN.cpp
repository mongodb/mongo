/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/ARN.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/logging/LogMacros.h>

namespace Aws
{
    namespace Utils
    {
        ARN::ARN(const Aws::String& arnString)
        {
            m_valid = false;

            // An ARN can be identified as any string starting with arn: with 6 defined segments each separated by a :
            const auto result = StringUtils::Split(arnString, ':', StringUtils::SplitOptions::INCLUDE_EMPTY_ENTRIES);

            if (result.size() < 6)
            {
                return;
            }

            if (result[0] != "arn")
            {
                return;
            }

            m_arnString = arnString;
            m_partition = result[1];
            m_service = result[2];
            m_region = result[3];
            m_accountId = result[4];
            m_resource = result[5];

            for (size_t i = 6; i < result.size(); i++)
            {
                m_resource += ":" + result[i];
            }

            m_valid = true;
        }
    }
}