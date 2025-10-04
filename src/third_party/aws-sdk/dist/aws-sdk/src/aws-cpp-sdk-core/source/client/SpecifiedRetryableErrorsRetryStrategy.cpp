/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/client/SpecifiedRetryableErrorsRetryStrategy.h>

#include <aws/core/client/AWSError.h>

using namespace Aws;
using namespace Aws::Client;

bool SpecifiedRetryableErrorsRetryStrategy::ShouldRetry(const AWSError<CoreErrors>& error, long attemptedRetries) const
{    
    if (attemptedRetries >= m_maxRetries)
    {
        return false;
    }
    for (const auto& err: m_specifiedRetryableErrors)
    {
        if (error.GetExceptionName() == err)
        {
            return true;
        }
    }

    return error.ShouldRetry();
}
