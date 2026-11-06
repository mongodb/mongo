// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

class BSONObjBuilder;

/**
 * Contains information to augment the 'ChangeStreamEndOfTransactionBackfillRequired' kickback
 * raised by the end-of-transaction synthesis stage on the router. The router catches the kickback
 * outside the change stream pipeline and transparently rebuilds the stream.
 *
 * It carries the timestamp from which the rebuilt stream must scan ('backfillFrom') and the resume
 * token of the accounting control event that required the lookback ('raisePosition').
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ChangeStreamEndOfTransactionBackfillRequiredInfo final
    : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::ChangeStreamEndOfTransactionBackfillRequired;

    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj& obj);

    ChangeStreamEndOfTransactionBackfillRequiredInfo(Timestamp backfillFrom, BSONObj raisePosition)
        : _backfillFrom(backfillFrom), _raisePosition(raisePosition.getOwned()) {}

    Timestamp getBackfillFrom() const {
        return _backfillFrom;
    }

    BSONObj getRaisePosition() const {
        return _raisePosition;
    }

    void serialize(BSONObjBuilder* bob) const final;

private:
    Timestamp _backfillFrom;
    BSONObj _raisePosition;
};

}  // namespace mongo
