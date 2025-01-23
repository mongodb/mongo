/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/client/DefaultRetryStrategy.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSVector.h>
namespace Aws
{
namespace Client
{

/**
 * @brief This retry strategy is almost identical to DefaultRetryStrategy, except it accepts a vector of error or exception names
 * that you want to retry anyway (bypass the retryable definition of the error instance itself) if the retry attempts is less than maxRetries.
 */
class AWS_CORE_API SpecifiedRetryableErrorsRetryStrategy : public DefaultRetryStrategy
{
public:
    SpecifiedRetryableErrorsRetryStrategy(const Aws::Vector<Aws::String>& specifiedRetryableErrors, long maxRetries = 10, long scaleFactor = 25) :
        DefaultRetryStrategy(maxRetries, scaleFactor), m_specifiedRetryableErrors(specifiedRetryableErrors)
    {}

    bool ShouldRetry(const AWSError<CoreErrors>& error, long attemptedRetries) const override;

private:
    Aws::Vector<Aws::String> m_specifiedRetryableErrors;
};

} // namespace Client
} // namespace Aws
