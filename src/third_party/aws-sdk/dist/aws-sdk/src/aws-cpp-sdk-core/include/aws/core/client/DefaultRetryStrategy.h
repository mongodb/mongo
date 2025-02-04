/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/client/RetryStrategy.h>

namespace Aws
{
namespace Client
{

class AWS_CORE_API DefaultRetryStrategy : public RetryStrategy
{
public:

    DefaultRetryStrategy(long maxRetries = 10, long scaleFactor = 25) :
        m_scaleFactor(scaleFactor), m_maxRetries(maxRetries)
    {}

    bool ShouldRetry(const AWSError<CoreErrors>& error, long attemptedRetries) const override;

    long CalculateDelayBeforeNextRetry(const AWSError<CoreErrors>& error, long attemptedRetries) const override;

    virtual long GetMaxAttempts() const override { return m_maxRetries + 1; }

    const char* GetStrategyName() const override { return "default";}

protected:
    long m_scaleFactor;
    long m_maxRetries;
};

} // namespace Client
} // namespace Aws
