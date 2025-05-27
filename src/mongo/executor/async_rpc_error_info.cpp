/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include "mongo/executor/async_rpc_error_info.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {
namespace {

MONGO_INIT_REGISTER_ERROR_EXTRA_INFO(AsyncRPCErrorInfo);

}  // namespace

std::shared_ptr<const ErrorExtraInfo> AsyncRPCErrorInfo::parse(const BSONObj& obj) {
    return std::make_shared<AsyncRPCErrorInfo>(
        Status(ErrorCodes::BadValue, "RemoteCommandExectionError illegally parsed from bson"));
}

void AsyncRPCErrorInfo::serialize(BSONObjBuilder* bob) const {}

namespace async_rpc {

Status unpackRPCStatus(Status status) {
    invariant(status == ErrorCodes::RemoteCommandExecutionError);
    auto errorInfo = status.extraInfo<AsyncRPCErrorInfo>();
    if (errorInfo->isLocal()) {
        return errorInfo->asLocal();
    }
    invariant(errorInfo->isRemote());
    auto remoteError = errorInfo->asRemote();
    Status out = remoteError.getRemoteCommandResult();
    if (out.isOK()) {
        out = remoteError.getRemoteCommandWriteConcernError();
    }
    if (out.isOK()) {
        out = remoteError.getRemoteCommandFirstWriteError();
    }
    return out;
}

Status unpackRPCStatusIgnoringWriteErrors(Status status) {
    invariant(status == ErrorCodes::RemoteCommandExecutionError);
    auto errorInfo = status.extraInfo<AsyncRPCErrorInfo>();
    if (errorInfo->isLocal()) {
        return errorInfo->asLocal();
    }
    invariant(errorInfo->isRemote());
    auto remoteError = errorInfo->asRemote();
    Status out = remoteError.getRemoteCommandResult();
    if (out.isOK()) {
        out = remoteError.getRemoteCommandWriteConcernError();
    }
    return out;
}

Status unpackRPCStatusIgnoringWriteConcernAndWriteErrors(Status status) {
    invariant(status == ErrorCodes::RemoteCommandExecutionError);
    auto errorInfo = status.extraInfo<AsyncRPCErrorInfo>();
    if (errorInfo->isLocal()) {
        return errorInfo->asLocal();
    }
    invariant(errorInfo->isRemote());
    auto remoteError = errorInfo->asRemote();
    return remoteError.getRemoteCommandResult();
}

}  // namespace async_rpc
}  // namespace mongo
