/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/core/client/DefaultRetryStrategy.h>

#include <aws/core/client/AWSError.h>
#include <aws/core/utils/UnreferencedParam.h>

using namespace Aws;
using namespace Aws::Client;

bool DefaultRetryStrategy::ShouldRetry(const AWSError<CoreErrors>& error, long attemptedRetries) const
{    
    if (attemptedRetries >= m_maxRetries)
        return false;

    return error.ShouldRetry();
}

long DefaultRetryStrategy::CalculateDelayBeforeNextRetry(const AWSError<CoreErrors>& error, long attemptedRetries) const
{
    AWS_UNREFERENCED_PARAM(error);

    if (attemptedRetries == 0)
    {
        return 0;
    }

    return (1UL << attemptedRetries) * m_scaleFactor;
}
