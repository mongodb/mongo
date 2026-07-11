// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string>
#include <string_view>

namespace mongo {


/**
 * Represents an error returned from the query stats system when it fails to add a recording for a
 * query. The extra info here can be helpful for our test infrastructure to decide how seriously to
 * treat it.
 */
class [[MONGO_MOD_PUBLIC]] QueryStatsFailedToRecordInfo final : public ErrorExtraInfo {
public:
    static constexpr auto code = ErrorCodes::QueryStatsFailedToRecord;

    static std::shared_ptr<const ErrorExtraInfo> parse(const BSONObj&);

    QueryStatsFailedToRecordInfo(BSONObj cmdObj, Status status, std::string_view versionString);

    void serialize(BSONObjBuilder* bob) const override;

    BSONObj toBSON() const {
        BSONObjBuilder bob;
        serialize(&bob);
        return bob.obj();
    }

private:
    BSONObj _cmdObj;
    Status _status;
    std::string _versionString;
};

}  // namespace mongo
