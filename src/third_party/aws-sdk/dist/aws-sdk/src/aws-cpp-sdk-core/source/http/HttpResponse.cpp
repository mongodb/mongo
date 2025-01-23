/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/http/HttpResponse.h>

#include <aws/core/utils/memory/stl/AWSStreamFwd.h>
#include <aws/core/utils/StringUtils.h>

namespace Aws
{
    namespace Http
    {
        /**
         * Overload ostream operator<< for HttpResponseCode enum class for a prettier output such as "200" and not "<C8-00 00-00>"
         */
        Aws::OStream& operator<< (Aws::OStream& oStream, HttpResponseCode code)
        {
            oStream << Aws::Utils::StringUtils::to_string(static_cast<typename std::underlying_type<HttpResponseCode>::type>(code));
            return oStream;
        }
    } // Http
} // Aws
