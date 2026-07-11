// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/query_stats_failed_to_record_info.h"

#include "mongo/base/error_extra_info.h"
#include "mongo/base/init.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/rpc/get_status_from_command_result.h"

#include <string_view>

namespace mongo {
namespace {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(QueryStatsFailedToRecordInfo);

}  // namespace

QueryStatsFailedToRecordInfo::QueryStatsFailedToRecordInfo(BSONObj cmdObj,
                                                           Status status,
                                                           std::string_view versionString)
    : _cmdObj(cmdObj.getOwned()), _status(std::move(status)), _versionString(versionString) {}

void QueryStatsFailedToRecordInfo::serialize(BSONObjBuilder* bob) const {
    bob->append("version", _versionString);
    bob->append("cmdObj", _cmdObj);
    BSONObjBuilder statusObjBuilder = bob->subobjStart("status");
    _status.serialize(&statusObjBuilder);
}

std::shared_ptr<const ErrorExtraInfo> QueryStatsFailedToRecordInfo::parse(const BSONObj& obj) {
    auto cmdObj = obj["cmdObj"].Obj();
    auto status = getStatusFromCommandResult(obj["status"].Obj());
    auto version = obj["version"].String();

    return std::make_shared<QueryStatsFailedToRecordInfo>(
        std::move(cmdObj), std::move(status), std::move(version));
}
}  // namespace mongo
