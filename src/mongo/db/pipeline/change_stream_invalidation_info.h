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
 * Contains information to augment the 'ChangeStreamInvalidated' error code. In particular, this
 * class holds the resume token of the "invalidate" event which gave rise to the exception.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ChangeStreamInvalidationInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::ChangeStreamInvalidated;

    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj& obj);

    explicit ChangeStreamInvalidationInfo(BSONObj invalidateToken)
        : _invalidateToken{invalidateToken.getOwned()} {}

    BSONObj getInvalidateResumeToken() const {
        return _invalidateToken;
    }

    void serialize(BSONObjBuilder* bob) const final;

private:
    BSONObj _invalidateToken;
};

}  // namespace mongo
