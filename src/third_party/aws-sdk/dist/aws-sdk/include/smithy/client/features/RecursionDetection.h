/**
* Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/platform/Environment.h>
#include <aws/core/http/HttpRequest.h>
#include <iomanip>

namespace smithy
{
    namespace client
    {
        static const char SMITHY_AWS_LAMBDA_FUNCTION_NAME[] = "AWS_LAMBDA_FUNCTION_NAME";
        static const char SMITHY_X_AMZN_TRACE_ID[] = "_X_AMZN_TRACE_ID";

        class RecursionDetection
        {
        public:
            static void AppendRecursionDetectionHeader(const std::shared_ptr<Aws::Http::HttpRequest>& ioRequest)
            {
                if (!ioRequest || ioRequest->HasHeader(Aws::Http::X_AMZN_TRACE_ID_HEADER))
                {
                    return;
                }
                const Aws::String& awsLambdaFunctionName = Aws::Environment::GetEnv(SMITHY_AWS_LAMBDA_FUNCTION_NAME);
                if (awsLambdaFunctionName.empty())
                {
                    return;
                }
                Aws::String xAmznTraceIdVal = Aws::Environment::GetEnv(SMITHY_X_AMZN_TRACE_ID);
                if (xAmznTraceIdVal.empty())
                {
                    return;
                }

                // Escape all non-printable ASCII characters by percent encoding
                Aws::OStringStream xAmznTraceIdValEncodedStr;
                for (const char ch : xAmznTraceIdVal)
                {
                    if (ch >= 0x20 && ch <= 0x7e) // ascii chars [32-126] or [' ' to '~'] are not escaped
                    {
                        xAmznTraceIdValEncodedStr << ch;
                    }
                    else
                    {
                        // A percent-encoded octet is encoded as a character triplet
                        xAmznTraceIdValEncodedStr << '%' // consisting of the percent character "%"
                            << std::hex << std::setfill('0') << std::setw(2) << std::uppercase
                            << (size_t)ch
                            //followed by the two hexadecimal digits representing that octet's numeric value
                            << std::dec << std::setfill(' ') << std::setw(0) << std::nouppercase;
                    }
                }
                xAmznTraceIdVal = xAmznTraceIdValEncodedStr.str();

                ioRequest->SetHeaderValue(Aws::Http::X_AMZN_TRACE_ID_HEADER, xAmznTraceIdVal);
            }
        };
    }
}
