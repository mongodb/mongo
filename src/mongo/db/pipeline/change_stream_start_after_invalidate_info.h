// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

class BSONObjBuilder;

/**
 * Contains information to augment the 'ChangeStreamStartAfterInvalidation' error code. In
 * particular, this class captures the 'invalidate' event that contains the client-provided resume
 * token.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ChangeStreamStartAfterInvalidateInfo final
    : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::ChangeStreamStartAfterInvalidate;

    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj& obj);

    explicit ChangeStreamStartAfterInvalidateInfo(BSONObj startAfterInvalidateEvent)
        : _startAfterInvalidateEvent{startAfterInvalidateEvent.getOwned()} {}

    BSONObj getStartAfterInvalidateEvent() const {
        return _startAfterInvalidateEvent;
    }

    void serialize(BSONObjBuilder* bob) const final;

private:
    BSONObj _startAfterInvalidateEvent;
};

}  // namespace mongo
