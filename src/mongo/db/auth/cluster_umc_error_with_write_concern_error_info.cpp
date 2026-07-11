// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/auth/cluster_umc_error_with_write_concern_error_info.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep

namespace mongo {
namespace {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(ClusterUMCErrorWithWriteConcernErrorInfo);

}  // namespace

ClusterUMCErrorWithWriteConcernErrorInfo::ClusterUMCErrorWithWriteConcernErrorInfo(
    Status mainError, WriteConcernErrorDetail writeConcernError)
    : _mainError(std::move(mainError)), _wcError(std::move(writeConcernError)) {}

std::shared_ptr<const ErrorExtraInfo> ClusterUMCErrorWithWriteConcernErrorInfo::parse(
    const BSONObj& doc) {
    uasserted(1004350,
              fmt::format("ClusterUMCErrorWithWriteConcernErrorInfo should never appear in command "
                          "result BSON object: {}",
                          doc.toString()));
    return {};
}

void ClusterUMCErrorWithWriteConcernErrorInfo::serialize(BSONObjBuilder* bob) const {
    _mainError.serialize(bob);
    bob->append("writeConcernError", _wcError.toBSON());
}

const Status& ClusterUMCErrorWithWriteConcernErrorInfo::getMainStatus() const {
    return _mainError;
}

const WriteConcernErrorDetail&
ClusterUMCErrorWithWriteConcernErrorInfo::getWriteConcernErrorDetail() const {
    return _wcError;
}

}  // namespace mongo
