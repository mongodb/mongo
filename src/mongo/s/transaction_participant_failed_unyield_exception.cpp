/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

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
