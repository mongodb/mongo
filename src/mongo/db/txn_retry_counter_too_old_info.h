// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

class [[MONGO_MOD_PUBLIC]] TxnRetryCounterTooOldInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::TxnRetryCounterTooOld;

    explicit TxnRetryCounterTooOldInfo(const TxnRetryCounter txnRetryCounter)
        : _txnRetryCounter(txnRetryCounter) {};

    const auto& getTxnRetryCounter() const {
        return _txnRetryCounter;
    }

    void serialize(BSONObjBuilder* bob) const override;
    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);

private:
    TxnRetryCounter _txnRetryCounter;
};

using TxnRetryCounterTooOldException = ExceptionFor<ErrorCodes::TxnRetryCounterTooOld>;

}  // namespace mongo
