/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/executor/async_rpc_retry_policy.h"
#include "mongo/s/client/shard.h"

namespace mongo {
namespace async_rpc {

namespace {
// See ARS::kMaxNumFailedHostRetryAttempts and Shard::kOnErrorNumRetries
const int kOnErrorNumRetries = 3;
}  // namespace

class ShardRetryPolicy : public RetryPolicy {
public:
    ShardRetryPolicy(Shard::RetryPolicy shardInternalRetryPolicy)
        : _shardInternalRetryPolicy(shardInternalRetryPolicy) {}

    virtual bool shouldRetry(Status s, int retryCount) {
        if (_retryCount >= kOnErrorNumRetries) {
            return false;
        }
        return Shard::remoteIsRetriableError(s.code(), _shardInternalRetryPolicy);
    }

    bool recordAndEvaluateRetry(Status s) override {
        bool shouldRetryVal = shouldRetry(s, _retryCount);

        if (shouldRetryVal) {
            ++_retryCount;
        }
        return shouldRetryVal;
    }

    Milliseconds getNextRetryDelay() override final {
        return Milliseconds::zero();
    }

    BSONObj toBSON() const override final {
        return BSON("retryPolicyType"
                    << "shardRetryPolicy");
    }

private:
    // The internal retry policy used by the Shard class.
    Shard::RetryPolicy _shardInternalRetryPolicy;

    // The number of retries that have been attempted by the corresponding async_rpc runner.
    int _retryCount = 0;
};

class ShardRetryPolicyWithIsStartingTransaction : public ShardRetryPolicy {
public:
    ShardRetryPolicyWithIsStartingTransaction(Shard::RetryPolicy shardInternalRetryPolicy,
                                              bool isStartingTransaction)
        : ShardRetryPolicy(shardInternalRetryPolicy),
          _isStartingTransaction(isStartingTransaction) {}

    bool shouldRetry(Status s, int retryCount) override {
        if (_isStartingTransaction) {
            return false;
        }
        return ShardRetryPolicy::shouldRetry(s, retryCount);
    }

private:
    bool _isStartingTransaction;
};

}  // namespace async_rpc
}  // namespace mongo
