// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/txn_retry_counter_too_old_info.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"

#include <string_view>

namespace mongo {

namespace {
using namespace std::literals::string_view_literals;

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(TxnRetryCounterTooOldInfo);

constexpr std::string_view kTxnRetryCounterFieldName = "txnRetryCounter"sv;

}  // namespace

void TxnRetryCounterTooOldInfo::serialize(BSONObjBuilder* bob) const {
    bob->append(kTxnRetryCounterFieldName, _txnRetryCounter);
}

std::shared_ptr<const ErrorExtraInfo> TxnRetryCounterTooOldInfo::parse(const BSONObj& obj) {
    return std::make_shared<TxnRetryCounterTooOldInfo>(obj[kTxnRetryCounterFieldName].Int());
}

}  // namespace mongo
