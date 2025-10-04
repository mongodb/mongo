/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/http/Scheme.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/StringUtils.h>

using namespace Aws::Http;
using namespace Aws::Utils;

namespace Aws
{
namespace Http
{
namespace SchemeMapper
{

    const char* ToString(Scheme scheme)
    {
        switch (scheme)
        {
            case Scheme::HTTP:
                return "http";
            case Scheme::HTTPS:
                return "https";
            default:
                return "http";
        }
    }

    Scheme FromString(const char* name)
    {
        Aws::String trimmedString = StringUtils::Trim(name);
        Aws::String loweredTrimmedString = StringUtils::ToLower(trimmedString.c_str());

        if (loweredTrimmedString == "http")
        {
            return Scheme::HTTP;
        }
        //this branch is technically unneeded, but it is here so we don't have a subtle bug
        //creep in as we extend this enum.
        else if (loweredTrimmedString == "https")
        {
            return Scheme::HTTPS;
        }

        return Scheme::HTTPS;
    }

} // namespace SchemeMapper
} // namespace Http
} // namespace Aws
