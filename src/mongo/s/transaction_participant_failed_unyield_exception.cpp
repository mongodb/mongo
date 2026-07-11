// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/transaction_participant_failed_unyield_exception.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/rpc/get_status_from_command_result.h"

namespace mongo {
namespace {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(TransactionParticipantFailedUnyieldInfo);

}  // namespace

void TransactionParticipantFailedUnyieldInfo::serialize(BSONObjBuilder* bob) const {
    BSONObjBuilder b;
    _originalError.serializeErrorToBSON(&b);
    bob->append(kOriginalErrorFieldName, b.obj());

    if (_originalResponseStatus) {
        BSONObjBuilder b2;
        _originalResponseStatus->serialize(&b2);
        bob->append(kOriginalResponseStatusFieldName, b2.obj());
    }
}

std::shared_ptr<const ErrorExtraInfo> TransactionParticipantFailedUnyieldInfo::parse(
    const BSONObj& obj) {
    auto originalError = getErrorStatusFromCommandResult(obj[kOriginalErrorFieldName].Obj());
    auto originalResponse = obj[kOriginalResponseStatusFieldName]
        ? boost::make_optional(
              getStatusFromCommandResult(obj[kOriginalResponseStatusFieldName].Obj()))
        : boost::optional<Status>();
    return std::make_shared<TransactionParticipantFailedUnyieldInfo>(originalError,
                                                                     originalResponse);
}

}  // namespace mongo
