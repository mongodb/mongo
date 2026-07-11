// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/logical_time.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/util/modules.h"

#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * The SignedLogicalTime class is a pair of value i.e. time and a signature i.e. _proof
 * The class is immutable and is used to hold the cryptographically protected LogicalTime.
 */
// TODO This class should be parent_private ideally
class [[MONGO_MOD_NEEDS_REPLACEMENT]] SignedLogicalTime {
public:
    using TimeProof = TimeProofService::TimeProof;

    SignedLogicalTime() = default;

    explicit SignedLogicalTime(LogicalTime time, long long keyId)
        : _time(std::move(time)), _keyId(keyId) {}

    explicit SignedLogicalTime(LogicalTime time, TimeProof proof, long long keyId)
        : _time(std::move(time)), _proof(std::move(proof)), _keyId(keyId) {}

    LogicalTime getTime() const {
        return _time;
    }

    boost::optional<TimeProof> getProof() const {
        return _proof;
    }

    long long getKeyId() const {
        return _keyId;
    }

    std::string toString() const;

    static const SignedLogicalTime kUninitialized;

private:
    LogicalTime _time;
    boost::optional<TimeProof> _proof;
    long long _keyId{0};
};

}  // namespace mongo
