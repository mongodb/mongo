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

#include "mongo/db/s/primary_only_service_helpers/primary_only_service_retry_strategy.h"

namespace mongo {
namespace primary_only_service_helpers {

namespace {
DefaultRetryStrategy::RetryParameters getRetryParameters() {
    auto base = DefaultRetryStrategy::getRetryParametersFromServerParameters();
    base.maxRetryAttempts = std::numeric_limits<std::int32_t>::max();
    return base;
}

PrimaryOnlyServiceRetryStrategy::RetryCriteria makeCriteriaAdapter(
    RetryabilityPredicate retryabilityPredicate) {
    return [retryabilityPredicate = std::move(retryabilityPredicate)](
               Status s, std::span<const std::string> errorLabels) {
        return retryabilityPredicate(s);
    };
}
}  // namespace


PrimaryOnlyServiceRetryStrategy::PrimaryOnlyServiceRetryStrategy(
    RetryabilityPredicate retryabilityPredicate,
    unique_function<void(const Status&)> onTransientError,
    unique_function<void(const Status&)> onUnrecoverableError)
    : _underlyingStrategy(std::make_unique<DefaultRetryStrategy>(
          makeCriteriaAdapter(std::move(retryabilityPredicate)), getRetryParameters())),
      _onTransientError(std::move(onTransientError)),
      _onUnrecoverableError(std::move(onUnrecoverableError)) {}

bool PrimaryOnlyServiceRetryStrategy::recordFailureAndEvaluateShouldRetry(
    Status s,
    const boost::optional<HostAndPort>& origin,
    std::span<const std::string> errorLabels) {
    auto willRetry =
        _underlyingStrategy->recordFailureAndEvaluateShouldRetry(s, origin, errorLabels);
    if (willRetry) {
        _onTransientError(s);
    } else {
        _onUnrecoverableError(s);
    }
    return willRetry;
}

void PrimaryOnlyServiceRetryStrategy::recordSuccess(const boost::optional<HostAndPort>& origin) {
    return _underlyingStrategy->recordSuccess(origin);
}

Milliseconds PrimaryOnlyServiceRetryStrategy::getNextRetryDelay() const {
    return _underlyingStrategy->getNextRetryDelay();
}

const TargetingMetadata& PrimaryOnlyServiceRetryStrategy::getTargetingMetadata() const {
    return _underlyingStrategy->getTargetingMetadata();
}

void PrimaryOnlyServiceRetryStrategy::recordBackoff(Milliseconds backoff) {
    return _underlyingStrategy->recordBackoff(backoff);
}

}  // namespace primary_only_service_helpers
}  // namespace mongo
