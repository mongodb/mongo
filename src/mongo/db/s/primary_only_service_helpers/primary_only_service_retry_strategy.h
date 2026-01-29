/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/client/retry_strategy.h"
#include "mongo/util/modules.h"

namespace MONGO_MOD_PUB mongo {
namespace primary_only_service_helpers {

using RetryabilityPredicate = std::function<bool(const Status&)>;

class PrimaryOnlyServiceRetryStrategy final : public RetryStrategy {
public:
    using RetryCriteria = DefaultRetryStrategy::RetryCriteria;

    PrimaryOnlyServiceRetryStrategy(RetryabilityPredicate retryabilityPredicate,
                                    unique_function<void(const Status&)> onTransientError,
                                    unique_function<void(const Status&)> onUnrecoverableError);
    [[nodiscard]]
    bool recordFailureAndEvaluateShouldRetry(Status s,
                                             const boost::optional<HostAndPort>& origin,
                                             std::span<const std::string> errorLabels) override;
    void recordSuccess(const boost::optional<HostAndPort>& origin) override;
    Milliseconds getNextRetryDelay() const override;
    const TargetingMetadata& getTargetingMetadata() const override;
    void recordBackoff(Milliseconds backoff) override;

private:
    std::unique_ptr<RetryStrategy> _underlyingStrategy;
    unique_function<void(const Status&)> _onTransientError;
    unique_function<void(const Status&)> _onUnrecoverableError;
};

}  // namespace primary_only_service_helpers
}  // namespace MONGO_MOD_PUB mongo
