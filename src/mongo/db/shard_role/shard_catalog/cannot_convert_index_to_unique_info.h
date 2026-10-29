// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Represents an error returned from the collMod command when an attempt to enforce the constraint
 * on an index fails because constraint violations exist.
 */
class [[MONGO_MOD_PUBLIC_FOR_TECHNICAL_REASONS]] CannotConvertIndexToUniqueInfo final
    : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::CannotConvertIndexToUnique;

    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);

    explicit CannotConvertIndexToUniqueInfo(const BSONArray& violations)
        : _violations(violations.getOwned()) {}

    void serialize(BSONObjBuilder* bob) const override;

    BSONObj toBSON() const {
        BSONObjBuilder bob;
        serialize(&bob);
        return bob.obj();
    }

private:
    // Includes the '_id' and fields that violate the index constraint for each document.
    BSONArray _violations;
};

}  // namespace mongo
