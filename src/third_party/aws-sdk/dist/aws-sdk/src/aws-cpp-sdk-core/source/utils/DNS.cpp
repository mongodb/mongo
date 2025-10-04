/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/utils/DNS.h>
#include <aws/core/utils/StringUtils.h>

namespace Aws
{
    namespace Utils
    {
        bool IsValidDnsLabel(const Aws::String& label)
        {
            // Valid DNS hostnames are composed of valid DNS labels separated by a period.
            // Valid DNS labels are characterized by the following:
            // 1- Only contains alphanumeric characters and/or dashes
            // 2- Cannot start or end with a dash
            // 3- Have a maximum length of 63 characters (the entirety of the domain name should be less than 255 bytes)

            if (label.empty())
                return false;

            if (label.size() > 63)
                return false;

            if (!StringUtils::IsAlnum(label.front()))
                return false; // '-' is not acceptable as the first character

            if (!StringUtils::IsAlnum(label.back()))
                return false; // '-' is not acceptable as the last  character

            for (size_t i = 1, e = label.size() - 1; i < e; ++i)
            {
                auto c = label[i];
                if (c != '-' && !StringUtils::IsAlnum(c))
                    return false;
            }

            return true;
        }

        bool IsValidHost(const Aws::String& host)
        {
            // Valid DNS hostnames are composed of valid DNS labels separated by a period.
            auto labels = StringUtils::Split(host, '.');
            if (labels.empty()) 
            {
                return false;
            }

            return !std::any_of(labels.begin(), labels.end(), [](const Aws::String& label){ return !IsValidDnsLabel(label); });
        }
    }
}
