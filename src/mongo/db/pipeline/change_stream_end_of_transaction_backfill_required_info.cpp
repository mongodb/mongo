// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/change_stream_end_of_transaction_backfill_required_info.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/assert_util.h"

#include <string_view>

namespace mongo {
namespace {

constexpr std::string_view kBackfillFromFieldName = "backfillFrom";
constexpr std::string_view kRaisePositionFieldName = "raisePosition";

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(ChangeStreamEndOfTransactionBackfillRequiredInfo);

}  // namespace

std::shared_ptr<const ErrorExtraInfo> ChangeStreamEndOfTransactionBackfillRequiredInfo::parse(
    const BSONObj& obj) {
    const auto backfillFrom = obj.getField(kBackfillFromFieldName);
    uassert(13877300,
            fmt::format("ChangeStreamEndOfTransactionBackfillRequiredInfo was missing '{}' field",
                        kBackfillFromFieldName),
            backfillFrom.type() == BSONType::timestamp);

    const auto raisePosition = obj.getField(kRaisePositionFieldName);
    uassert(13877301,
            fmt::format("ChangeStreamEndOfTransactionBackfillRequiredInfo was missing '{}' field",
                        kRaisePositionFieldName),
            raisePosition.type() == BSONType::object);

    return std::make_shared<ChangeStreamEndOfTransactionBackfillRequiredInfo>(
        backfillFrom.timestamp(), raisePosition.Obj().getOwned());
}

void ChangeStreamEndOfTransactionBackfillRequiredInfo::serialize(BSONObjBuilder* bob) const {
    bob->append(kBackfillFromFieldName, _backfillFrom);
    bob->append(kRaisePositionFieldName, _raisePosition);
}

}  // namespace mongo
